"""P351 地址发现：只读 DSCI / CFGQ，绝不改 IP、RF 或采集配置。

导入模块不会枚举网卡或访问网络。自动模式仅探测已连接的物理有线
网卡；手填本机 IPv4 时只探测该接口（包括用户明确选择的无线接口）。
DSCA 只是候选，必须再由 p351_board_config 的严格 V9 校验确认身份。
协议没有密码学认证；本模块防止旧包/串包，不声称抵御恶意仿冒固件。
"""
from dataclasses import dataclass
import ctypes
import ipaddress
import math
import os
import secrets
import select
import socket
import struct
import time

import p351_board_config

PORT = 40000
DEFAULT_BOARD = "192.168.10.2"
MAX_INTERFACES = 8
MAX_RESPONDERS = 16


@dataclass(frozen=True)
class LocalInterface:
    local_ip: str
    name: str
    is_ethernet: bool = True
    prefix_length: int = 24


@dataclass(frozen=True)
class DiscoveryResult:
    board_ip: str
    local_ip: str
    config: p351_board_config.BoardConfig
    rtt_ms: float
    interface_name: str


class DiscoveryError(RuntimeError):
    def __init__(self, message, diagnostics=()):
        super().__init__(message)
        self.diagnostics = tuple(diagnostics)


class MultipleBoardsError(DiscoveryError):
    def __init__(self, results):
        self.results = tuple(results)
        super().__init__("发现多个 P351，拒绝自动选择：" +
                         "、".join(item.board_ip for item in results) +
                         "。请仅连接目标板卡后重试。")


def _valid_host(value):
    """只接受规范十进制单播 IPv4；允许隔离网口的 169.254 地址。"""
    try:
        address = ipaddress.IPv4Address(value)
    except (ValueError, TypeError):
        return False
    return (str(address) == value and not address.is_loopback
            and not address.is_multicast and not address.is_unspecified
            and 0 < int(address) >> 24 < 224
            and int(address) != 0xFFFFFFFF)


def _windows_interfaces():
    """使用 Win32 IP Helper，无 CIM、PowerShell、psutil 或管理员权限。

    布局依照 Microsoft IP_ADAPTER_ADDRESSES_LH（截至 OperStatus）、
    IP_ADAPTER_UNICAST_ADDRESS_LH 和 MIB_IF_ROW2 官方定义；DWORD/ULONG
    固定为 32 bit，不能用平台相关 c_ulong 替代。每个版本节点检查 Length。
    https://learn.microsoft.com/windows/win32/api/iptypes/ns-iptypes-ip_adapter_addresses_lh
    https://learn.microsoft.com/windows/win32/api/netioapi/ns-netioapi-mib_if_row2
    """
    u32, u16, u8, ptr = ctypes.c_uint32, ctypes.c_uint16, ctypes.c_ubyte, ctypes.c_void_p

    class SocketAddress(ctypes.Structure):
        _fields_ = [("address", ptr), ("length", ctypes.c_int32)]

    class Unicast(ctypes.Structure):
        pass

    Unicast._fields_ = [
        ("Length", u32), ("Flags", u32), ("Next", ctypes.POINTER(Unicast)),
        ("Address", SocketAddress), ("PrefixOrigin", u32), ("SuffixOrigin", u32),
        ("DadState", u32), ("ValidLifetime", u32), ("PreferredLifetime", u32),
        ("LeaseLifetime", u32), ("OnLinkPrefixLength", u8),
    ]

    class Adapter(ctypes.Structure):
        pass

    Adapter._fields_ = [
        ("Length", u32), ("IfIndex", u32), ("Next", ctypes.POINTER(Adapter)),
        ("AdapterName", ctypes.c_char_p), ("FirstUnicastAddress", ctypes.POINTER(Unicast)),
        ("FirstAnycastAddress", ptr), ("FirstMulticastAddress", ptr),
        ("FirstDnsServerAddress", ptr), ("DnsSuffix", ctypes.c_wchar_p),
        ("Description", ctypes.c_wchar_p), ("FriendlyName", ctypes.c_wchar_p),
        ("PhysicalAddress", u8 * 8), ("PhysicalAddressLength", u32),
        ("Flags", u32), ("Mtu", u32), ("IfType", u32), ("OperStatus", u32),
    ]

    class Guid(ctypes.Structure):
        _fields_ = [("Data1", u32), ("Data2", u16), ("Data3", u16), ("Data4", u8 * 8)]

    class IfRow(ctypes.Structure):
        _fields_ = [
            ("Luid", ctypes.c_uint64), ("Index", u32), ("Guid", Guid),
            ("Alias", u16 * 257), ("Description", u16 * 257),
            ("PhysicalAddressLength", u32), ("PhysicalAddress", u8 * 32),
            ("PermanentPhysicalAddress", u8 * 32), ("Mtu", u32), ("Type", u32),
            ("TunnelType", u32), ("MediaType", u32), ("PhysicalMediumType", u32),
            ("AccessType", u32), ("DirectionType", u32), ("StatusFlags", u8),
            ("OperStatus", u32), ("AdminStatus", u32), ("MediaConnectState", u32),
            ("NetworkGuid", Guid), ("ConnectionType", u32),
            ("TransmitLinkSpeed", ctypes.c_uint64), ("ReceiveLinkSpeed", ctypes.c_uint64),
            ("Counters", ctypes.c_uint64 * 18),
        ]

    helper = ctypes.WinDLL("iphlpapi.dll")
    gaa = helper.GetAdaptersAddresses
    gaa.argtypes = [u32, u32, ptr, ptr, ctypes.POINTER(u32)]
    gaa.restype = u32
    get_if = helper.GetIfEntry2
    get_if.argtypes = [ctypes.POINTER(IfRow)]
    get_if.restype = u32
    size = u32(15000)  # Microsoft recommended initial working-buffer size.
    for _ in range(3):
        if size.value > 16 * 1024 * 1024:
            raise OSError("网卡信息缓冲区异常")
        buffer = ctypes.create_string_buffer(size.value)
        code = gaa(socket.AF_INET, 0x02 | 0x04 | 0x08, None, buffer, ctypes.byref(size))
        if code == 111:  # ERROR_BUFFER_OVERFLOW, adapters may have changed.
            continue
        if code == 232:  # ERROR_NO_DATA
            return []
        if code:
            raise OSError(code, "GetAdaptersAddresses 失败")
        break
    else:
        raise OSError("网卡状态持续变化，请稍后重试")

    result, seen_nodes = [], set()
    node = ctypes.cast(buffer, ctypes.POINTER(Adapter))
    while node:
        location = ctypes.addressof(node.contents)
        if location in seen_nodes or len(seen_nodes) >= 256:
            raise OSError("网卡链表异常")
        seen_nodes.add(location)
        item = node.contents
        if item.Length < Adapter.OperStatus.offset + ctypes.sizeof(u32):
            raise OSError("Windows 网卡结构版本不支持")
        if item.OperStatus == 1 and item.IfType not in (24, 131):
            row = IfRow()
            row.Index = item.IfIndex
            # HardwareInterface, connector present, media connected, not filter.
            physical = (get_if(ctypes.byref(row)) == 0 and row.StatusFlags & 1
                        and row.StatusFlags & 4 and not row.StatusFlags & (2 | 16)
                        and row.OperStatus == 1 and row.Type == 6)
            unicast, seen_addresses = item.FirstUnicastAddress, set()
            while unicast:
                address_location = ctypes.addressof(unicast.contents)
                if address_location in seen_addresses or len(seen_addresses) >= 256:
                    raise OSError("网卡地址链表异常")
                seen_addresses.add(address_location)
                entry = unicast.contents
                if entry.Length < Unicast.Address.offset + ctypes.sizeof(SocketAddress):
                    raise OSError("Windows IPv4 地址结构版本不支持")
                if entry.Address.address and entry.Address.length >= 8:
                    raw = ctypes.string_at(entry.Address.address, 8)
                    ip = socket.inet_ntoa(raw[4:8])
                    # sockaddr_in family is native little endian on Windows.
                    if struct.unpack_from("<H", raw)[0] == socket.AF_INET and _valid_host(ip):
                        prefix = (int(entry.OnLinkPrefixLength)
                                  if entry.Length > Unicast.OnLinkPrefixLength.offset else 32)
                        if 0 <= prefix <= 32:
                            result.append(LocalInterface(ip, item.FriendlyName or
                                                         item.Description or "网络接口",
                                                         bool(physical), prefix))
                unicast = entry.Next
        node = item.Next
    return result


def _posix_interfaces():
    """Linux 的本地 ioctl 只读枚举；未知平台保守地不自动广播。"""
    if not os.path.isdir("/sys/class/net"):
        return []
    import fcntl
    result = []
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        for _, name in socket.if_nameindex():
            request = struct.pack("256s", name.encode()[:15])
            try:
                flags = struct.unpack_from("H", fcntl.ioctl(probe, 0x8913, request), 16)[0]
                if not flags & 1 or flags & 8 or not flags & 0x40:
                    continue  # IFF_UP, !IFF_LOOPBACK, IFF_RUNNING.
                ip = socket.inet_ntoa(fcntl.ioctl(probe, 0x8915, request)[20:24])
                mask = socket.inet_ntoa(fcntl.ioctl(probe, 0x891B, request)[20:24])
                prefix = ipaddress.IPv4Network("0.0.0.0/" + mask).prefixlen
                physical = (os.path.exists("/sys/class/net/" + name + "/device")
                            and not os.path.isdir("/sys/class/net/" + name + "/wireless"))
                if _valid_host(ip):
                    result.append(LocalInterface(ip, name, physical, prefix))
            except (OSError, ValueError):
                continue
    return result


def _all_interfaces():
    return _windows_interfaces() if os.name == "nt" else _posix_interfaces()


def candidate_interfaces():
    """返回可自动探测的已连接物理有线 IPv4，不主动发送任何网络包。"""
    try:
        available = _all_interfaces()
    except (OSError, AttributeError) as exc:
        raise DiscoveryError("读取本机网卡失败，请检查网卡状态。", (str(exc),)) from exc
    selected = {item.local_ip: item for item in available if item.is_ethernet}
    return sorted(selected.values(), key=lambda item: (item.name, item.local_ip))


def _selected_interfaces(preferred_local):
    if not preferred_local:
        selected = candidate_interfaces()
    else:
        if not _valid_host(preferred_local):
            raise DiscoveryError("本机地址必须是有效的本机单播 IPv4。")
        try:
            selected = [item for item in _all_interfaces() if item.local_ip == preferred_local]
        except (OSError, AttributeError) as exc:
            raise DiscoveryError("无法读取指定本机网卡。", (str(exc),)) from exc
        if not selected:
            raise DiscoveryError("本机地址未属于已连接的网卡；请检查地址或接好网线。")
        selected = selected[:1]
    if not selected:
        raise DiscoveryError("未检测到已连接的物理以太网 IPv4；请给 P351 上电并接好网线。"
                             "不会自动探测 Wi-Fi/VPN。")
    if len(selected) > MAX_INTERFACES:
        raise DiscoveryError("可用有线地址过多，请在本机地址栏指定连接 P351 的 IPv4。")
    return selected


def _usable_sender(ip, interface):
    if not _valid_host(ip) or ip == interface.local_ip:
        return False
    network = ipaddress.IPv4Network(f"{interface.local_ip}/{interface.prefix_length}", strict=False)
    # Cross-subnet DSCI replies may be impossible to route; do not reject their
    # real source merely because the board retains a different static subnet.
    return ipaddress.IPv4Address(ip) not in (network.network_address, network.broadcast_address)


def discover_boards(preferred_local="", previous_board=DEFAULT_BOARD, *, timeout=3.0):
    """在总预算内读取所有匹配 P351，失败抛 DiscoveryError。

    仅一次/接口有限广播 DSCI；CFGQ 只访问有效 DSCA 来源、上次地址和
    默认地址，不进行子网扫描。结果已通过严格 V9 配置解析；同一 IP
    多接口重复回包只保留最快一条。网络等待预算最长 timeout 秒。
    """
    if not isinstance(timeout, (int, float)) or not math.isfinite(timeout) or not .1 <= timeout <= 10:
        raise ValueError("发现超时必须为 0.1～10 秒")
    interfaces = _selected_interfaces(preferred_local)
    deadline = time.monotonic() + timeout
    nonce = secrets.randbits(32)
    expected = struct.pack("<4sII", b"DSCA", nonce, 0)
    request = struct.pack("<4sI", b"DSCI", nonce)
    sockets, diagnostics, candidates, results = {}, [], set(), {}
    unverified_candidates = set()
    try:
        for interface in interfaces:
            sock = None
            try:
                sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                sock.settimeout(max(.001, deadline - time.monotonic()))
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
                sock.bind((interface.local_ip, 0))
                sock.sendto(request, ("255.255.255.255", PORT))
                sockets[sock] = interface
            except OSError as exc:
                diagnostics.append(f"{interface.name}/{interface.local_ip}: {exc}")
                if sock is not None:
                    sock.close()
        if not sockets:
            raise DiscoveryError("本机网口绑定/广播失败；请检查地址、网线及防火墙。", diagnostics)

        listen_until = min(deadline, time.monotonic() + min(.8, timeout * .35))
        while time.monotonic() < listen_until:
            try:
                ready, _, _ = select.select(list(sockets), [], [], max(0, listen_until - time.monotonic()))
            except OSError as exc:
                raise DiscoveryError("网卡在查址过程中断开，请重新连接后测试。", (str(exc),)) from exc
            if not ready:
                break
            for sock in ready:
                try:
                    data, sender = sock.recvfrom(2048)
                except (OSError, socket.timeout):
                    continue
                if (len(sender) == 2 and sender[1] == PORT and data == expected
                        and _usable_sender(sender[0], sockets[sock])):
                    candidates.add((sock, sender[0]))
                    if len(candidates) > MAX_RESPONDERS:
                        raise DiscoveryError("发现回包过多，拒绝自动选择；请使用独立网线连接目标板卡。")

        # Actual broadcast responders always precede the bounded legacy fallback.
        work = sorted(candidates, key=lambda pair: (sockets[pair[0]].local_ip, pair[1]))
        known = {value for value in (previous_board, DEFAULT_BOARD) if _valid_host(value)}
        for sock, interface in sockets.items():
            for ip in sorted(known):
                pair = (sock, ip)
                if _usable_sender(ip, interface) and pair not in candidates:
                    work.append(pair)
        for index, (sock, board_ip) in enumerate(work):
            remaining = deadline - time.monotonic()
            if remaining <= .005:
                if index < len(candidates):
                    raise DiscoveryError("候选板卡身份核验未完成，拒绝自动选择；请减少接入板卡后重试。", diagnostics)
                break
            query_timeout = min(.35, remaining)
            started = time.monotonic()
            try:
                config = p351_board_config.query_on_socket(
                    sock, (board_ip, PORT), timeout=query_timeout, attempts=1)
            except (TimeoutError, ValueError, OSError) as exc:
                diagnostics.append(f"{board_ip} 经 {sockets[sock].local_ip}: {exc}")
                if ((sock, board_ip) in candidates and not isinstance(exc, ValueError)):
                    unverified_candidates.add(board_ip)
                continue
            interface = sockets[sock]
            result = DiscoveryResult(board_ip, interface.local_ip, config,
                                     max(0.0, (time.monotonic() - started) * 1000), interface.name)
            if board_ip not in results or result.rtt_ms < results[board_ip].rtt_ms:
                results[board_ip] = result
        unresolved = unverified_candidates.difference(results)
        if results and unresolved:
            # A valid DSCA with no conclusive CFGQ is not evidence that this is
            # a different product. Never silently choose the one faster board.
            raise DiscoveryError("部分候选板卡身份核验超时，拒绝自动选择：" +
                                 "、".join(sorted(unresolved)) +
                                 "。请仅连接目标板卡后重试。", diagnostics)
        if not results:
            raise DiscoveryError("未取得匹配的 P351 V9 身份回包；请检查板卡上电、网线、固件和防火墙。"
                                 "默认地址不是实测结果，未修改板卡配置。", diagnostics)
        return sorted(results.values(), key=lambda result: tuple(map(int, result.board_ip.split("."))))
    finally:
        for sock in sockets:
            sock.close()


def choose_single(preferred_local="", previous_board=DEFAULT_BOARD, *, timeout=3.0):
    """UI 单击入口：多板不猜测，不改旧地址，不发任何控制命令。"""
    results = discover_boards(preferred_local, previous_board, timeout=timeout)
    if len(results) != 1:
        raise MultipleBoardsError(results)
    return results[0]


def netcfg_reassign(board_ip, new_ip, mask="255.255.255.0", gateway="0.0.0.0", *,
                    local_ip="", timeout=2.0):
    """同网段工具：把板卡 IP 临时改为 new_ip 并在同一子网内确认固化。

    帧：NCIP+nonce+ip+mask+gw（20B 载荷，单播到板卡当前 IP:40000）→ NCAK(nonce,result)
    确认：NCCF+nonce（源=PC）→ NCCK。30 秒内必须 NCCF，否则板卡回滚原 IP。
    跨网段使用需板卡固件放开广播 NCIP（待 Vitis，见交接文档 §18）。
    """
    import secrets as _secrets
    nonce = _secrets.randbits(32)
    ipb = bytes(int(x) for x in new_ip.split('.'))
    mb = bytes(int(x) for x in mask.split('.'))
    gb = bytes(int(x) for x in gateway.split('.'))
    bind_ip = local_ip or board_ip  # 同网段场景下直接绑定本机接口地址
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(0.4)
    try:
        payload = b"NCIP" + struct.pack('<I', nonce) + ipb + mb + gb
        sock.sendto(payload, (board_ip, PORT))
        ack = None
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline and ack is None:
            try:
                data, addr = sock.recvfrom(64)
            except socket.timeout:
                break
            if len(data) >= 12 and data[:4] == b"NCAK" and struct.unpack('<I', data[4:8])[0] == nonce:
                ack = (data[8] if len(data) >= 9 else 0, addr)
        if ack is None:
            raise DiscoveryError(f"NCFG 无应答：板卡 {board_ip} 未确认 trial IP {new_ip}。", ())
        if ack[0] != 0:
            raise DiscoveryError(f"NCFG 被拒绝（result={ack[0]}：1=参数/2=已有trial）。", ())
        # 确认固化（板卡已切到 new_ip，同一子网可达）
        time.sleep(0.1)
        sock2 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock2.settimeout(0.8)
        try:
            sock2.sendto(b"NCCF" + struct.pack('<I', nonce), (new_ip, PORT))
            ck = None
            deadline2 = time.monotonic() + 1.5
            while time.monotonic() < deadline2 and ck is None:
                try:
                    data, addr = sock2.recvfrom(64)
                except socket.timeout:
                    break
                if len(data) >= 12 and data[:4] == b"NCCK" and struct.unpack('<I', data[4:8])[0] == nonce:
                    ck = data[8] if len(data) >= 9 else 1
            if ck is None:
                raise DiscoveryError("NCCF 确认无应答：板卡 30 秒后将回滚原 IP，请重试。", ())
            if ck != 0:
                raise DiscoveryError("NCCF 被拒绝（nonce/owner 不匹配）。", ())
        finally:
            sock2.close()
        return True
    finally:
        sock.close()
