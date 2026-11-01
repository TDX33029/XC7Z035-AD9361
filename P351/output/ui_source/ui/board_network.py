"""Board discovery and transactional runtime IPv4 configuration (no Flash writes)."""
import ipaddress
import secrets
import socket
import struct
import time

PORT = 40000


def validate_config(address, mask, gateway, local):
    interface = ipaddress.IPv4Interface(f"{address}/{mask}")
    ip, network = interface.ip, interface.network
    host = ipaddress.IPv4Address(local)
    gw = ipaddress.IPv4Address(gateway)
    if (ip.is_multicast or ip.is_loopback or int(ip) >> 24 == 0 or int(ip) >> 24 >= 224
            or network.prefixlen not in range(1, 31)
            or ip in (network.network_address, network.broadcast_address, host)):
        raise ValueError("板卡地址/掩码无效或与本机冲突")
    if host not in network:
        raise ValueError("自动确认要求本机已位于目标子网；请先配置本机网卡或附加地址")
    if int(gw) and (gw not in network or gw in (ip, network.network_address, network.broadcast_address)):
        raise ValueError("网关必须为目标子网内独立的主机地址，或填0.0.0.0")
    return ip.packed + network.netmask.packed + gw.packed


def discover(local, duration=1.5):
    nonce = secrets.randbits(32)
    found = set()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.bind((local, 0))
        sock.sendto(struct.pack("<4sI", b"DSCI", nonce), ("255.255.255.255", PORT))
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            sock.settimeout(max(.001, deadline-time.monotonic()))
            try:
                data, endpoint = sock.recvfrom(256)
            except socket.timeout:
                break
            if endpoint[1] == PORT and data == struct.pack("<4sII", b"DSCA", nonce, 0):
                found.add(endpoint[0])
    return sorted(found)


def configure(old, local, address, mask, gateway):
    payload = validate_config(address, mask, gateway, local)
    nonce = secrets.randbits(32)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind((local, 0))

        def exchange(target, packet, magic, attempts):
            for _ in range(attempts):
                sock.sendto(packet, (target, PORT))
                deadline = time.monotonic()+.7
                while time.monotonic() < deadline:
                    sock.settimeout(max(.001, deadline-time.monotonic()))
                    try:
                        data, endpoint = sock.recvfrom(256)
                    except socket.timeout:
                        break
                    if endpoint != (target, PORT) or len(data) != 12:
                        continue
                    tag, token, result = struct.unpack("<4sII", data)
                    if tag != magic or token != nonce:
                        continue
                    if result:
                        raise ValueError(f"板卡拒绝网络配置：状态{result}")
                    return
            raise TimeoutError("新地址尚未确认；请等待30秒回退后重新发现，勿立即断定改址失败")

        try:
            exchange(old, struct.pack("<4sI", b"NCIP", nonce)+payload, b"NCAK", 1)
        except TimeoutError:
            pass  # The old-address ACK may have been lost after successful apply.
        exchange(address, struct.pack("<4sI", b"NCCF", nonce), b"NCCK", 3)
    return address
