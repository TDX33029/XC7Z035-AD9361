"""Confirmed board reference-clock switching, independent of the BIT payload."""
import secrets
import struct

from tx_bit_protocol import exchange_ack


def confirm_reference(sock, endpoint, config: bytes) -> str:
    magic, version, source, reserved, rate = struct.unpack("<4sBBHI", config)
    if (magic != b"REFC" or version != 1 or reserved or
            (source, rate) not in ((0, 40_000_000), (1, 10_000_000))):
        raise ValueError("参考时钟仅支持内部40 MHz或外部10 MHz")
    nonce = secrets.randbits(32)
    packet = struct.pack("<4sBBHII", b"REFC", 2, source, 0, rate, nonce)
    try:
        result = exchange_ack(sock, endpoint, packet, b"RFAK", nonce,
                              timeout=8.0, retries=2)
    except TimeoutError as exc:
        raise TimeoutError("参考时钟未收到板端确认；请更新PS程序并检查网络，尚未继续发射") from exc
    if result:
        raise ValueError({
            1: "板端拒绝参考时钟参数",
            2: "当前PL未包含时钟选源控制，请更新BIT/XSA后重试",
            3: "停止采集/发射失败，时钟切换未完成",
            4: "参考时钟配置或锁定校验失败；检查10 MHz输入，或切回内部40 MHz恢复",
        }.get(result, f"参考时钟切换失败，状态码{result}"))
    if source:
        return "板端已确认：外部10 MHz参考，内部40 MHz关闭控制已核验；实际停振需实测"
    return "板端已确认：内部40 MHz参考"
