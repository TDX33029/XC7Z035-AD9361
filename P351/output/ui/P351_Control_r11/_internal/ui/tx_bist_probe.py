"""Temporary AD9361 TX-chain isolation tone; not a normal waveform mode.

Run while the selected physical TX (TX2 in the current firmware) is already
in 2 MHz CW mode at >=20 dB AD9361 attenuation. Logical slot 1 stays disabled.
``on``, ``on12``, ``on6``, and ``on0`` inject -18/-12/-6/0 dB tones for a
distortion-versus-level check. The tone enters after the AD9361 data port
and expires after 30 seconds. Keep the spectrum analyzer input protected.
"""

from __future__ import annotations

import argparse
import secrets
import socket
import struct


REQUEST = struct.Struct("<4sII")
RESPONSE = struct.Struct("<4sII")
ACTIONS = {"off": 0, "on": 1, "status": 2,
           "on0": 3, "on6": 4, "on12": 5}
LEVELS_DB = {"on": 18, "on12": 12, "on6": 6, "on0": 0}


def build_request(action: str, nonce: int) -> bytes:
    return REQUEST.pack(b"TBDG", nonce, ACTIONS[action])


def parse_response(packet: bytes, nonce: int) -> tuple[bool, int]:
    if len(packet) != RESPONSE.size:
        raise ValueError("invalid diagnostic reply length")
    magic, returned_nonce, result = RESPONSE.unpack(packet)
    if magic != b"TBDA" or returned_nonce != nonce:
        raise ValueError("diagnostic reply magic/nonce mismatch")
    return bool(result & 0x00010000), result & 0xFFFF


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=ACTIONS)
    parser.add_argument("--board", default="192.168.10.2")
    args = parser.parse_args()
    nonce = secrets.randbits(32)
    request = build_request(args.action, nonce)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.settimeout(1.0)
        for _ in range(3):
            sock.sendto(request, (args.board, 40000))
            try:
                packet, peer = sock.recvfrom(64)
            except socket.timeout:
                continue
            if peer[0] != args.board or peer[1] != 40000:
                continue
            try:
                active, error = parse_response(packet, nonce)
            except ValueError:
                continue
            if error:
                print(f"拒绝诊断：错误码 {error}。请确认当前物理TX（本版TX2）为正弦模式、第二逻辑槽关闭、衰减至少20 dB。")
                return 1
            if not active:
                print("芯片内部单音：关闭；恢复 PL I/Q 输入。")
            elif args.action == "status":
                print("芯片内部单音：已开启；用 off 可提前关闭。")
            else:
                print(f"芯片内部单音：开启（-{LEVELS_DB[args.action]} dB 电平）；"
                      "目标频点为 LO + 1.92 MHz，30 秒后自动关闭。")
            return 0
    print("未收到诊断确认；检查板端是否已更新 ELF、网口与板卡地址。")
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
