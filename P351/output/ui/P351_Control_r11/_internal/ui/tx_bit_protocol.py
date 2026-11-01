"""Self-contained copy of the Zynq TX BIT UDP protocol for portable UI use."""

from __future__ import annotations

import enum
import struct
import zlib
import socket
import time
import secrets
from dataclasses import dataclass


MAGIC = b"TXB1"
VERSION = 1
MAX_DATA_PAYLOAD = 1024
HEADER = struct.Struct("<4sBBHIBBBBIIHHIII")
HEADER_SIZE = HEADER.size


def exchange_ack(sock, endpoint, payload, magic, nonce, *, timeout=0.35, retries=3):
    """Bounded stop-and-wait; ignore stale replies and other senders."""
    for _ in range(retries):
        sock.sendto(payload, endpoint)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            sock.settimeout(max(0.001, deadline - time.monotonic()))
            try:
                reply, sender = sock.recvfrom(2048)
            except socket.timeout:
                break
            if sender != endpoint or len(reply) != 12:
                continue
            tag, token, result = struct.unpack("<4sII", reply)
            if tag == magic and token == nonce:
                return result
    raise TimeoutError("板端确认超时：请检查网络并更新 PS 固件；未确认发射成功")


def send_bit_confirmed(sock, endpoint, packet):
    payload = packet.encode()
    result = exchange_ack(sock, endpoint, payload, b"TBAK",
                          struct.unpack_from("<I", payload, 36)[0])
    if result:
        raise ValueError(f"板端拒绝 BIT 命令 {packet.opcode.name}（状态 {result}）")


def wait_bit_state(sock, endpoint, task, *, started=False, timeout=3.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        nonce = secrets.randbits(32)
        state = exchange_ack(sock, endpoint, struct.pack("<4sII", b"BQRY", nonce, task),
                             b"BQAK", nonce, retries=1)
        if state & 32 and not (state & 8):
            raise ValueError("板端 BIT/DMA 启动或运行错误；请查看状态日志")
        if not (state & 8) and ((state & 70) == 70 if started else not (state & 16)):
            return state
        time.sleep(0.02)
    raise TimeoutError("板端 BIT 状态未达到预期，未确认发射成功")


class Opcode(enum.IntEnum):
    BEGIN = 1
    DATA = 2
    COMMIT = 3
    START = 4
    STOP = 5
    CLEAR = 6
    STATUS = 7


class Channel(enum.IntEnum):
    TX1 = 1
    TX2 = 2
    BOTH_SAME = 3


class BitOrder(enum.IntEnum):
    MSB_FIRST = 0


@dataclass(frozen=True)
class TxBitPacket:
    opcode: Opcode
    task_id: int
    channel: Channel = Channel.TX1
    profile: int = 0
    bit_order: BitOrder = BitOrder.MSB_FIRST
    flags: int = 0
    total_bits: int = 0
    offset_bytes: int = 0
    repeat_count: int = 0
    bit_rate: int = 0
    payload: bytes = b""

    def encode(self) -> bytes:
        if len(self.payload) > MAX_DATA_PAYLOAD:
            raise ValueError("UDP BIT payload exceeds 1024 bytes")
        numeric = (self.flags, self.task_id, self.profile, self.total_bits,
                   self.offset_bytes, self.repeat_count, self.bit_rate)
        if any(value < 0 or value > 0xFFFFFFFF for value in numeric):
            raise ValueError("packet field is outside its unsigned range")
        header = HEADER.pack(
            MAGIC, VERSION, int(self.opcode), self.flags & 0xFFFF,
            self.task_id, int(self.channel), self.profile & 0xFF,
            int(self.bit_order), 0, self.total_bits, self.offset_bytes,
            len(self.payload), 0, self.repeat_count, self.bit_rate, 0)
        checksum = zlib.crc32(header[:-4] + self.payload) & 0xFFFFFFFF
        return header[:-4] + struct.pack("<I", checksum) + self.payload

    @staticmethod
    def decode(datagram: bytes) -> "TxBitPacket":
        if len(datagram) < HEADER_SIZE:
            raise ValueError("truncated TXB1 header")
        fields = HEADER.unpack(datagram[:HEADER_SIZE])
        (magic, version, opcode, flags, task_id, channel, profile,
         bit_order, _reserved0, total_bits, offset_bytes, payload_len,
         _reserved1, repeat_count, bit_rate, received_crc) = fields
        if magic != MAGIC or version != VERSION:
            raise ValueError("unsupported BIT protocol magic/version")
        if len(datagram) != HEADER_SIZE + payload_len:
            raise ValueError("BIT datagram length does not match payload_len")
        payload = datagram[HEADER_SIZE:]
        calculated_crc = zlib.crc32(datagram[:HEADER_SIZE - 4] + payload) & 0xFFFFFFFF
        if calculated_crc != received_crc:
            raise ValueError("BIT datagram CRC32 mismatch")
        return TxBitPacket(
            opcode=Opcode(opcode), task_id=task_id, channel=Channel(channel),
            profile=profile, bit_order=BitOrder(bit_order), flags=flags,
            total_bits=total_bits, offset_bytes=offset_bytes,
            repeat_count=repeat_count, bit_rate=bit_rate, payload=payload)


def make_download_packets(payload: bytes, *, valid_bits: int, task_id: int,
                          channel: Channel = Channel.TX1, profile: int = 0,
                          bit_rate: int = 800_000,
                          repeat_count: int = 0) -> list[TxBitPacket]:
    """Build BEGIN, ordered DATA fragments and COMMIT for one transfer."""
    if not 0 <= valid_bits <= len(payload) * 8:
        raise ValueError("valid_bits exceeds the supplied payload")
    common = dict(task_id=task_id, channel=channel, profile=profile,
                  total_bits=valid_bits, repeat_count=repeat_count,
                  bit_rate=bit_rate)
    packets = [TxBitPacket(Opcode.BEGIN, **common)]
    for offset in range(0, len(payload), MAX_DATA_PAYLOAD):
        packets.append(TxBitPacket(
            Opcode.DATA, offset_bytes=offset,
            payload=payload[offset:offset + MAX_DATA_PAYLOAD], **common))
    packets.append(TxBitPacket(Opcode.COMMIT, **common))
    return packets
