"""AD9361TX control UI for the Zynq PS UDP protocols.

The control path uses Python's standard library. NumPy and Matplotlib are
used for the raw TX-DAC time-domain and spectrum monitor (no demodulator).
"""
from __future__ import annotations

import ipaddress
import hashlib
import math
import queue
import secrets
import socket
import struct
import sys
import threading
import time
import tkinter as tk
import traceback
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

try:
    import matplotlib
    import numpy as np
    from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
    from matplotlib.figure import Figure
    matplotlib.rcParams["font.sans-serif"] = [
        "Microsoft YaHei", "SimHei", "Arial Unicode MS", "DejaVu Sans"]
    matplotlib.rcParams["axes.unicode_minus"] = False
except ImportError:
    matplotlib = None
    np = None
    FigureCanvasTkAgg = None
    Figure = None


# Support all three launch layouts: the development tree, a copied ``ui``
# directory, and a PyInstaller bundle.  Portable releases place the shared
# protocol beside this module; the development tree keeps the canonical copy
# in ``reference_model``.
UI_DIR = Path(__file__).resolve().parent
ROOT = UI_DIR.parent
# Insert the development fallback first, then the local portable copy so the
# latter has precedence when both are present.
for _module_dir in (ROOT / "reference_model", UI_DIR):
    if _module_dir.is_dir():
        _module_text = str(_module_dir)
        while _module_text in sys.path:
            sys.path.remove(_module_text)
        sys.path.insert(0, _module_text)

from tx_bit_protocol import (Channel, Opcode, TxBitPacket, make_download_packets,
                             send_bit_confirmed, wait_bit_state)
from ila_csv_analysis import analyze_ila_csv, format_ila_analysis
from iq_monitor_analysis import (analyze_monitor_iq, estimate_occupied_bandwidth,
                                 spectral_peak_summary)
from tfs_control import frequency_units, attenuation_units, control as tfs_control_action
from refclk_control import confirm_reference
from board_config import read_config, confirm_rf_config


CTRL_PORT = 40000
DATA_PORT = 40001
IQ_SAMPLE_RATE_HZ = 61_440_000
DATA_CLK_HZ = 122_880_000
MONITOR_REFRESH_HZ = 5
MONITOR_TIME_WINDOW_US = 10.0
MAX_BIT_RATE = IQ_SAMPLE_RATE_HZ
PL_REPLAY_BYTES = 16 * 1024
TASK_RESET_SETTLE_S = 0.020
BIT_COMMIT_SETTLE_S = 0.050
MODE_CONFIG_SETTLE_S = 0.100
MONITOR_QUIESCE_SETTLE_S = 0.100
REFCLK_SETTLE_S = 0.250
UI_BUILD = "2026.10.05-r68-pcm8-rnrz-audit"
FIXED_BOARD_IP = "192.168.10.2"
MODE_VALUE = {"不发射": 0, "QPSK（旧DDS）": 1, "FM（旧DDS）": 2,
              "SOQPSK-TG（PL）": 3, "PCM/FM（PL）": 4,
              "正弦波/CW（PL）": 6}
VISIBLE_MODE_VALUES = ("不发射", "PCM/FM（PL）", "SOQPSK-TG（PL）",
                       "正弦波/CW（PL）")
CHANNEL_VALUE = {"TX1": Channel.TX1}
PROFILE_VALUE = {"RAW BIT / ILA验证": 0, "DEBUG QPSK（预留）": 1,
                 "PCM/FM 标准4阶Bessel（连续码率）": 2,
                 "SOQPSK-TG": 4}
SOURCE_VALUES = ("HEX", "BIN文件", "PN9", "PN11", "PN15", "PN20", "PN23",
                 "PRBS7", "PRBS15", "全0", "全1", "0101")
PN_POLYNOMIALS = {
    "PN9": (9, 4, "x^9+x^5+1"),
    "PN11": (11, 2, "x^11+x^9+1"),
    "PN15": (15, 1, "x^15+x^14+1"),
    "PN20": (20, 17, "x^20+x^3+1"),
    "PN23": (23, 5, "x^23+x^18+1"),
}
STANDARD_BIT_MODES = ("PCM/FM（PL）", "SOQPSK-TG（PL）")
PCM_PROFILE = "PCM/FM 标准4阶Bessel（连续码率）"
PCM_MIN_BIT_RATE = 10_000
STANDARD_MAX_BIT_RATE = 20_000_000
PCM_MAX_BIT_RATE = STANDARD_MAX_BIT_RATE
SOQPSK_MIN_BIT_RATE = PCM_MIN_BIT_RATE
SOQPSK_MAX_BIT_RATE = STANDARD_MAX_BIT_RATE
AD9361_TX_BW_MIN_MHZ = 0.2
AD9361_TX_BW_MAX_MHZ = 56.0
TX_ATTENUATION_MAX_DB = 89.75
TX_ATTENUATION_STEP_DB = 0.25
OCCUPIED_BW_PRESETS_MHZ = (0.5, 1.0, 2.0, 3.3, 4.0, 6.0, 8.0,
                            10.0, 13.0, 15.0, 20.0, 30.0, 40.0)
AD9361_BW_PRESETS_MHZ = (0.2, *OCCUPIED_BW_PRESETS_MHZ, 56.0)
OCCUPIED_BW_FACTOR = {
    "PCM/FM（PL）": 1.16,
    "SOQPSK-TG（PL）": 0.78,
}
LINE_CODE_VALUE = {
    "NRZ-L": 0,
    "NRZ-M": 1,
    "NRZ-S": 2,
    "Biφ-L（Manchester）": 3,
    "Biφ-M": 4,
    "Biφ-S": 5,
    "RNRZ-L（IRIG随机化）": 6,
}
ILA_LINE_CODE_CURRENT = "使用发送页当前PCM码型"
ROLLOFF_VALUE = {"标准/无（PCM/FM Bessel）": 0, "RRC α=0.20": 1,
                 "RRC α=0.35": 2, "RRC α=0.50": 3,
                 "RRC α=0.75": 4, "RRC α=1.00": 5}
REFERENCE_VALUE = {"内部40 MHz晶振": (0, 40_000_000),
                   "外部10 MHz参考": (1, 10_000_000)}
MONITOR_SOURCE_VALUE = {
    "TX DAC I/Q（推荐）": (1, False),
    "RX ADC·外接SMA自收": (0, False),
}
LOOPBACK_CHANNEL_VALUE = {"通道1（TX1/RX1）": 0}
# Matplotlib redraw runs in Tk's UI thread.  15/20 FPS brings no useful
# spectrum resolution improvement but can starve button events on long runs.
LOOPBACK_REFRESH_VALUES = (1, 2, 5, 10)
ZSDR_HEADER = struct.Struct("<4sBBHIHHHHIII")


@dataclass(frozen=True)
class ZsdrDatagram:
    capture_id: int
    frame_index: int
    frame_count: int
    n_groups: int
    flags: int
    timestamp: int
    payload: bytes


def loopback_control_packet(enabled: bool) -> bytes:
    """Build the versioned AD9361 internal TX->RX loopback command."""
    return struct.pack("<4sBBH", b"LBCK", 1, 1 if enabled else 0, 0)


def monitor_source_packet(source: int) -> bytes:
    """Select the PL capture source: 0=RX ADC, 1=TX DAC."""
    if source not in (0, 1):
        raise ValueError("监视数据源必须为RX ADC(0)或TX DAC(1)")
    return struct.pack("<4sBBH", b"MONS", 1, source, 0)


def rf_rx_control_packet(enabled: bool) -> bytes:
    """Enable physical RX RF tuning for a TX-to-RX external coax test."""
    return struct.pack("<4sBBH", b"RFRX", 1, 1 if enabled else 0, 0)


def parse_zsdr_datagram(datagram: bytes) -> ZsdrDatagram:
    """Validate one UDP/40001 ZSDR v1 fragment."""
    if len(datagram) < ZSDR_HEADER.size:
        raise ValueError("ZSDR数据报短于32字节头")
    (magic, version, flags, packet_len, capture_id, frame_index,
     frame_count, n_groups, reserved0, ts_lo, ts_hi,
     reserved1) = ZSDR_HEADER.unpack_from(datagram)
    if magic != b"ZSDR" or version != 1:
        raise ValueError("不是ZSDR v1数据报")
    if packet_len != len(datagram):
        raise ValueError("ZSDR长度字段与UDP载荷不一致")
    if reserved0 != 0 or reserved1 != 0:
        raise ValueError("ZSDR保留字段非零")
    if frame_count == 0 or frame_index >= frame_count:
        raise ValueError("ZSDR分片编号无效")
    if not 1 <= n_groups <= 160:
        raise ValueError("ZSDR样本组数无效")
    payload = datagram[ZSDR_HEADER.size:]
    if len(payload) != n_groups * 8:
        raise ValueError("ZSDR I/Q载荷长度无效")
    return ZsdrDatagram(
        capture_id=capture_id,
        frame_index=frame_index,
        frame_count=frame_count,
        n_groups=n_groups,
        flags=flags,
        timestamp=(ts_hi << 32) | ts_lo,
        payload=payload,
    )


def assemble_zsdr_capture(frames: dict[int, ZsdrDatagram]) -> bytes:
    """Return ordered int16 IQ payload after validating a complete capture."""
    if not frames:
        raise ValueError("没有ZSDR分片")
    first = next(iter(frames.values()))
    if len(frames) != first.frame_count:
        raise ValueError("ZSDR采集帧不完整")
    ordered: list[bytes] = []
    for index in range(first.frame_count):
        frame = frames.get(index)
        if frame is None or frame.capture_id != first.capture_id or \
                frame.frame_count != first.frame_count or \
                frame.timestamp != first.timestamp:
            raise ValueError("ZSDR采集帧编号不连续")
        if frame.flags & 0x02:
            raise ValueError("ZSDR采集帧报告DMA错误")
        is_last = index == first.frame_count - 1
        if bool(frame.flags & 0x01) != is_last:
            raise ValueError("ZSDR END标志位置无效")
        ordered.append(frame.payload)
    return b"".join(ordered)


def detect_local_ip(board: str) -> str:
    """Ask the OS routing table for the source address; no UDP packet is sent."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.connect((board, CTRL_PORT))
            address = ipaddress.IPv4Address(probe.getsockname()[0])
            if address.is_unspecified or address.is_loopback or address.is_multicast or str(address) == board:
                return ""
            return str(address)
    except (OSError, ValueError):
        return ""  # Operator must select an address belonging to this computer.


def line_code_ui_state(mode: str | None) -> tuple[str, str]:
    """Return combobox state and operator guidance for the selected mode."""
    if mode == "PCM/FM（PL）":
        return ("readonly",
                "PCM/FM输入编码可选：NRZ-L/M/S、RNRZ-L、Biφ-L/M/S；"
                "本机IRIG PCM/FM验收使用RNRZ-L（IRIG15+NRZ-L）；"
                "其余为码型兼容测试，不代表满足第2章随机化与PCM/FM波形要求。")
    if mode == "SOQPSK-TG（PL）":
        return ("disabled",
                "SOQPSK-TG不使用PCM码型选择：固定采用IRIG随机化、差分编码和SOQPSK三元预编码。")
    return ("disabled", "只有PCM/FM模式使用此PCM码型选择。")


def validate_repeat_payload_size(payload_bytes: int, repeat_count: int) -> None:
    """Repeated jobs must fit in the v1.1 PL replay BRAM."""
    if repeat_count != 0 and payload_bytes > PL_REPLAY_BYTES:
        raise ValueError(
            f"有限/无限循环帧最多{PL_REPLAY_BYTES} byte；当前为{payload_bytes} byte。"
            "请缩短循环帧，或改为单次长帧。")


def recommended_parameters(mode: str, current_profile: str = "") -> dict[str, object] | None:
    """Return the operator-visible recommended values for a modulation mode.

    These values only populate the UI.  They are not transmitted until the
    operator presses Apply or Download and Start.
    """
    if mode == "PCM/FM（PL）":
        bit_rate = 5_000_000
        return {
            "profile": PCM_PROFILE,
            "profile_choices": (PCM_PROFILE,),
            "bit_rate": bit_rate,
            "deviation_hz": int(0.35 * bit_rate),
            "offset_hz": 0,
            "amplitude": "0.80",
            "differential": True,
            "line_code": "RNRZ-L（IRIG随机化）",
            "rolloff": "标准/无（PCM/FM Bessel）",
            "description": ("PCM/FM：RNRZ-L标准推荐，10 kbit/s–20 Mbit/s连续可调，4阶Bessel预调制脉冲，"
                            f"fc=0.7Rb；当前Rb={bit_rate / 1e6:g} Mbit/s，"
                            f"Δf=0.35Rb={0.35 * bit_rate / 1e6:g} MHz"),
        }
    if mode == "SOQPSK-TG（PL）":
        return {
            "profile": "SOQPSK-TG",
            "profile_choices": ("SOQPSK-TG",),
            "bit_rate": 5_000_000,
            "deviation_hz": 0,
            "offset_hz": 0,
            "amplitude": "0.80",
            "differential": True,
            "line_code": "RNRZ-L（IRIG随机化）",
            "rolloff": "标准/无（PCM/FM Bessel）",
            "description": "SOQPSK-TG：10 kbit/s–20 Mbit/s连续可调，固定IRIG随机化、差分编码和SOQPSK预编码；5 Mbit/s起始值，基带偏移0 Hz，幅度0.80",
        }
    return None


def parse_hex_text(value: str) -> bytes:
    cleaned = value.replace("0x", "").replace("0X", "")
    for char in " \t\r\n_,-:":
        cleaned = cleaned.replace(char, "")
    if not cleaned:
        raise ValueError("HEX数据不能为空")
    if len(cleaned) % 2:
        raise ValueError("HEX必须包含完整字节（偶数个十六进制字符）")
    try:
        return bytes.fromhex(cleaned)
    except ValueError as exc:
        raise ValueError("HEX中包含非十六进制字符") from exc


def generate_pattern(kind: str, bit_count: int) -> bytes:
    if not 0 < bit_count <= 8 * 1024 * 1024:
        raise ValueError("BIT数量必须在1到8388608之间")
    byte_count = (bit_count + 7) // 8
    if kind == "全0":
        return bytes(byte_count)
    if kind == "全1":
        return bytes([0xFF]) * byte_count
    if kind == "0101":
        return bytes([0x55]) * byte_count
    if kind in PN_POLYNOMIALS:
        return generate_pn_pattern(kind, bit_count)
    if kind not in ("PRBS7", "PRBS15"):
        raise ValueError(f"不支持的数据源：{kind}")

    width = 7 if kind == "PRBS7" else 15
    state = (1 << width) - 1
    result = bytearray(byte_count)
    for index in range(bit_count):
        output = state & 1
        if output:
            result[index // 8] |= 1 << (7 - index % 8)
        feedback = ((state >> 0) ^ (state >> 1)) & 1
        state = (state >> 1) | (feedback << (width - 1))
    return bytes(result)


def generate_pn_pattern(
    kind: str,
    bit_count: int,
    *,
    seed: int | None = None,
    invert: bool = False,
) -> bytes:
    """Generate a maximal-length PN stream, packed MSB-first.

    IRIG-106 lists PN11 and PN15 among the supported test patterns but does
    not fix transmitter phase.  This implementation exposes the non-zero seed
    and inversion so a downstream receiver can be configured identically.
    """
    if kind not in PN_POLYNOMIALS:
        raise ValueError(f"不支持的PN序列：{kind}")
    if not 0 < bit_count <= 8 * 1024 * 1024:
        raise ValueError("BIT数量必须在1到8388608之间")
    width, second_tap, _ = PN_POLYNOMIALS[kind]
    maximum = (1 << width) - 1
    state = maximum if seed is None else seed
    if not 0 < state <= maximum:
        raise ValueError(f"{kind}初始值必须在1到0x{maximum:X}之间，不能全0")
    result = bytearray((bit_count + 7) // 8)
    for index in range(bit_count):
        output = (state & 1) ^ int(invert)
        if output:
            result[index // 8] |= 1 << (7 - index % 8)
        feedback = ((state >> 0) ^ (state >> second_tap)) & 1
        state = (state >> 1) | (feedback << (width - 1))
    return bytes(result)


def _bit_from_payload(payload: bytes, valid_bits: int, index: int) -> int:
    source_index = index % valid_bits
    return (payload[source_index // 8] >> (7 - source_index % 8)) & 1


def build_minor_frame(
    payload: bytes,
    payload_valid_bits: int,
    *,
    sync_word: int,
    sync_bits: int,
    frame_bytes: int,
) -> tuple[bytes, int]:
    """Build one fixed-length PCM minor frame, transmitted MSB-first."""
    if not payload or not 0 < payload_valid_bits <= len(payload) * 8:
        raise ValueError("帧载荷为空或有效BIT数无效")
    if not 16 <= sync_bits <= 33:
        raise ValueError("IRIG-106小帧同步字长度必须为16–33 bit")
    if not 0 <= sync_word < (1 << sync_bits):
        raise ValueError(f"同步字超出{sync_bits} bit范围")
    if not 1 <= frame_bytes <= 2048:
        raise ValueError("小帧长度必须为1–2048 byte")
    frame_bits = frame_bytes * 8
    if frame_bits <= sync_bits:
        raise ValueError("小帧长度必须大于同步字长度")
    result = bytearray(frame_bytes)
    for index in range(sync_bits):
        bit = (sync_word >> (sync_bits - 1 - index)) & 1
        if bit:
            result[index // 8] |= 1 << (7 - index % 8)
    for index in range(sync_bits, frame_bits):
        if _bit_from_payload(payload, payload_valid_bits, index - sync_bits):
            result[index // 8] |= 1 << (7 - index % 8)
    return bytes(result), frame_bits


def build_id_major_frame(payload: bytes, payload_valid_bits: int, *,
                         sync_word: int, sync_bits: int, frame_bytes: int,
                         frame_count: int, sfid_start: int = 0) -> tuple[bytes, int]:
    """IRIG 106-22 4.3.2(c): fixed 8-bit SFID immediately after sync.

    One major frame contains 2..256 minor frames. SFID counts from 0 or 1;
    payload continues across minor frames, restarting only on replay.
    This is an 8-bit-word format, not a general-purpose subcommutator. The
    sync word itself may be 16..33 bits and need not end on a byte boundary.
    """
    if not 2 <= frame_count <= 256:
        raise ValueError("ID副帧数必须为2–256；1表示不使用ID副帧")
    if sfid_start not in (0, 1) or sfid_start + frame_count - 1 > 255:
        raise ValueError("8 bit SFID起始值只能为0或1；256小帧时必须从0开始")
    if not 1 <= frame_bytes <= 2048 or frame_bytes * frame_count > PL_REPLAY_BYTES:
        raise ValueError("ID大帧必须不超过16 KiB循环缓存；请减少副帧数或小帧长度")
    capacity = frame_bytes * 8 - sync_bits - 8
    if capacity <= 0:
        raise ValueError("小帧必须容纳同步字、8 bit SFID及数据载荷")
    # Reuse all existing sync/source validation before constructing a major frame.
    build_minor_frame(payload, payload_valid_bits, sync_word=sync_word,
                      sync_bits=sync_bits, frame_bytes=frame_bytes)
    if payload_valid_bits > capacity * frame_count:
        raise ValueError("输入数据超过ID大帧载荷容量，不能静默截断")
    result = bytearray(frame_bytes * frame_count)
    for frame in range(frame_count):
        start_bit = frame * frame_bytes * 8
        for index in range(sync_bits):
            if (sync_word >> (sync_bits - 1 - index)) & 1:
                target = start_bit + index
                result[target // 8] |= 1 << (7 - target % 8)
        sfid = frame + sfid_start
        for index in range(8):
            if (sfid >> (7 - index)) & 1:
                target = start_bit + sync_bits + index
                result[target // 8] |= 1 << (7 - target % 8)
        data_start = start_bit + sync_bits + 8
        for index in range(capacity):
            if _bit_from_payload(payload, payload_valid_bits, frame * capacity + index):
                target = data_start + index
                result[target // 8] |= 1 << (7 - target % 8)
    return bytes(result), len(result) * 8


def normalize_tail(payload: bytes, valid_bits: int) -> bytes:
    if not payload:
        raise ValueError("发送数据不能为空")
    if not 0 < valid_bits <= len(payload) * 8:
        raise ValueError(f"有效BIT数必须在1到{len(payload) * 8}之间")
    used_bytes = (valid_bits + 7) // 8
    result = bytearray(payload[:used_bytes])
    unused = (-valid_bits) & 7
    if unused:
        result[-1] &= (0xFF << unused) & 0xFF
    return bytes(result)


def bit_timing(bit_rate: int, bit_count: int) -> tuple[float, float]:
    """Return average I/Q samples per bit and one-pass duration in seconds."""
    if bit_rate <= 0:
        raise ValueError("BIT率必须大于0")
    if bit_count <= 0:
        raise ValueError("BIT数量必须大于0")
    return IQ_SAMPLE_RATE_HZ / bit_rate, bit_count / bit_rate


def pn_randomizer_conflict(source: str, pcm: bool, soqpsk: bool,
                           line_code: str) -> bool:
    active = soqpsk or (pcm and line_code == "RNRZ-L（IRIG随机化）")
    return bool(active and source in ("PN15", "PRBS15"))


def occupied_bandwidth_hz(mode: str, bit_rate: int) -> float:
    """Return the digital 99%-occupied-bandwidth engineering estimate."""
    if bit_rate <= 0:
        raise ValueError("BIT率必须大于0")
    try:
        factor = OCCUPIED_BW_FACTOR[mode]
    except KeyError as exc:
        raise ValueError("当前模式没有冻结的B99换算模型") from exc
    return factor * bit_rate


def bit_rate_for_occupied_bandwidth(mode: str, bandwidth_hz: float) -> int:
    """Return the nearest integer BIT rate for a requested B99 target."""
    if bandwidth_hz <= 0:
        raise ValueError("目标调制带宽必须大于0")
    try:
        factor = OCCUPIED_BW_FACTOR[mode]
    except KeyError as exc:
        raise ValueError("当前模式没有冻结的B99换算模型") from exc
    return int(round(bandwidth_hz / factor))


def bit_rate_limits(mode: str) -> tuple[int, int]:
    if mode == "PCM/FM（PL）":
        return PCM_MIN_BIT_RATE, PCM_MAX_BIT_RATE
    if mode == "SOQPSK-TG（PL）":
        return SOQPSK_MIN_BIT_RATE, SOQPSK_MAX_BIT_RATE
    raise ValueError("当前模式不支持目标调制带宽反算")


def format_bandwidth_choice(value_mhz: float) -> str:
    return f"{value_mhz:g} MHz"


CW_MIN_ATTENUATION_DB = 10.0  # Provisional RF-linearity guard; verify on hardware.


def power_to_attenuation_db(target_dbm: float, zero_attenuation_dbm: float) -> float:
    """Convert a calibrated absolute-power request to an AD9361 attenuation.

    The AD9361 controls relative attenuation, not connector power.  The
    zero-attenuation value must therefore be measured at the active LO and RF
    path.  Quantize to the device's 0.25 dB attenuation step before sending.
    """
    if not math.isfinite(target_dbm) or not math.isfinite(zero_attenuation_dbm):
        raise ValueError("TX功率和0 dB校准功率必须是有限数值")
    attenuation = zero_attenuation_dbm - target_dbm
    if attenuation < -1e-9 or attenuation > TX_ATTENUATION_MAX_DB + 1e-9:
        minimum = zero_attenuation_dbm - TX_ATTENUATION_MAX_DB
        raise ValueError(
            f"目标功率范围为{minimum:g}–{zero_attenuation_dbm:g} dBm（由当前校准值决定）")
    steps = int(math.floor(attenuation / TX_ATTENUATION_STEP_DB + 0.5))
    return min(steps * TX_ATTENUATION_STEP_DB, TX_ATTENUATION_MAX_DB)


def attenuation_to_power_dbm(attenuation_db: float,
                             zero_attenuation_dbm: float) -> float:
    return zero_attenuation_dbm - attenuation_db


def validate_cw_power(mode: str, attenuation_db: float, zero_dbm: float) -> None:
    if MODE_VALUE.get(mode) == 6 and attenuation_db < CW_MIN_ATTENUATION_DB:
        raise ValueError(
            f"单音线性保护：AD9361衰减至少{CW_MIN_ATTENUATION_DB:g} dB，"
            f"目标功率请设为≤{zero_dbm - CW_MIN_ATTENUATION_DB:g} dBm。"
            "该限制用于降低大功率杂散，仍需频谱仪验收。")


def read_chip_temperature(board: str, local: str) -> float:
    """Read a nonce-matched AD9361 estimate; never interpret timeout as zero."""
    nonce = secrets.randbits(32)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind((local, 0))
        sock.sendto(struct.pack("<4sI", b"TMPQ", nonce), (board, CTRL_PORT))
        deadline = time.monotonic() + 0.6
        while time.monotonic() < deadline:
            sock.settimeout(max(0.001, deadline - time.monotonic()))
            data, endpoint = sock.recvfrom(256)
            if endpoint != (board, CTRL_PORT) or len(data) != 12:
                continue
            magic, received, value = struct.unpack("<4sIi", data)
            if magic != b"TMPA" or received != nonce:
                continue
            if value == -2147483648 or not 0 < value < 224000:
                raise ValueError("传感器未就绪/读数异常")
            return value / 1000.0
    raise TimeoutError("温度应答超时")


def udp_round_trip(board: str, local: str, command: bytes = b"PING",
                   expected: bytes = b"PONG", *, timeout: float = 0.6,
                   attempts: int = 3) -> float:
    """Nonce-matched, bounded probe; ACK proves only communication/reset."""
    nonce = secrets.randbits(32)
    packet = struct.pack("<4sI", command, nonce)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind((local, 0))
        for _ in range(attempts):
            started = time.monotonic()
            sock.sendto(packet, (board, CTRL_PORT))
            deadline = started + timeout
            while time.monotonic() < deadline:
                sock.settimeout(max(0.001, deadline - time.monotonic()))
                try:
                    reply, endpoint = sock.recvfrom(2048)
                except socket.timeout:
                    break
                if endpoint != (board, CTRL_PORT) or len(reply) != 12:
                    continue
                magic, received_nonce, status = struct.unpack("<4sII", reply)
                if magic != expected or received_nonce != nonce:
                    continue
                if status:
                    raise RuntimeError(f"板端通信重置失败，状态码={status}")
                return (time.monotonic() - started) * 1000.0
    raise TimeoutError(
        "未收到匹配应答：检查新ELF、本机所选网卡IP、板卡IP及网络可达性、"
        "网线和防火墙。旧ELF无PONG/NRAK，超时不等于指令一定无法下发。")


def read_board_u32(board: str, local: str, command: bytes,
                   expected: bytes) -> int:
    """Read a nonce-matched RF setting; never substitute an editable UI value."""
    nonce = secrets.randbits(32)
    packet = struct.pack("<4sI", command, nonce)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind((local, 0))
        for _ in range(3):
            sock.sendto(packet, (board, CTRL_PORT))
            deadline = time.monotonic() + 0.6
            while time.monotonic() < deadline:
                sock.settimeout(max(0.001, deadline - time.monotonic()))
                try:
                    reply, endpoint = sock.recvfrom(2048)
                except socket.timeout:
                    break
                if endpoint == (board, CTRL_PORT) and len(reply) == 12:
                    magic, received_nonce, value = struct.unpack("<4sII", reply)
                    if magic == expected and received_nonce == nonce:
                        return value
    raise TimeoutError(f"板端未应答 {command.decode('ascii')}；请确认已下载新版ELF")


class ChoiceBar(ttk.Frame):
    """Keyboard-accessible choices sharing the existing protocol variable.

    Selecting is local only; the caller explicitly supplies any local callback.
    Programmatic variable changes update selection without invoking commands.
    """
    def __init__(self, parent, *, variable, values, labels=None, command=None,
                 columns=None, state="normal", width=10):
        super().__init__(parent)
        self.buttons = []
        labels = labels or {}
        columns = columns or len(values)
        for index, value in enumerate(values):
            button = ttk.Radiobutton(
                self, text=labels.get(value, value), value=value, variable=variable,
                command=command, style="Choice.TRadiobutton", width=width)
            button.grid(row=index // columns, column=index % columns,
                        sticky="ew", padx=(0, 4), pady=2)
            self.buttons.append(button)
            self.columnconfigure(index % columns, weight=1, uniform="choice")
        self.configure(state=state)

    def configure(self, cnf=None, **kwargs):
        state = kwargs.pop("state", None)
        if state is not None:
            for button in self.buttons:
                button.state(["disabled"] if state == "disabled" else ["!disabled"])
        return super().configure(cnf, **kwargs)


class ControlUI(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("AD9361TX 遥测信号发射控制台")
        self.geometry("1440x980")
        self.minsize(1180, 820)
        self.configure(background="#f3f6fa")

        self.board_ip = tk.StringVar(value=FIXED_BOARD_IP)
        self.local_ip = tk.StringVar(value=detect_local_ip(self.board_ip.get()))
        self.connection_state = tk.StringVar(value="未测试")
        self.operation_state = tk.StringVar(value="待机")
        # 界面默认针对本RX2/TX2版本；接通后以CFGQ读回为准，支持旧RX1/TX1板端。
        self.physical_rx_channel = 2
        self.physical_tx_channel = 2
        self.rf_ports_verified = False
        self.rf_channel_title = tk.StringVar(value="TX2（1R1T，待回读）")
        self.monitor_channel_title = tk.StringVar(value="TX2 / RX2（待回读）")
        self.monitor_route_hint = tk.StringVar(value=
            "固定 5 frame/s · 61.44 MSPS · 时域0–10 µs。外接SMA：TX2A→RX2A，需固定衰减器；端口待回读。")

        self.tx1_mode = tk.StringVar(value="不发射")
        self.tx2_mode = tk.StringVar(value="不发射")
        self.tx_lo_mhz = tk.StringVar(value="2400.0")
        self.tx_bw_mhz = tk.StringVar(value="20.0")
        self.rf_bw_summary = tk.StringVar(
            value="AD9361 TX模拟滤波带宽：0.2–56 MHz；它不是调制信号占用带宽。")
        self.reference_source = tk.StringVar(value="内部40 MHz晶振")
        self.reference_status = tk.StringVar(value="参考时钟尚未向板端确认")
        # Absolute output power is derived from a spectrum-analyser calibration
        # of the connector power at 0 dB AD9361 attenuation.  The defaults keep
        # the user-requested reference; this is not a measurement guarantee.
        # Both channels must be calibrated at their actual frequency/RF path.
        self.tx1_power_dbm = tk.StringVar(value="-10.0")
        self.tx2_power_dbm = tk.StringVar(value="-10.0")
        self.tx1_zero_attenuation_dbm = tk.StringVar(value="20.0")
        self.tx2_zero_attenuation_dbm = tk.StringVar(value="20.0")
        self.tx1_power_summary = tk.StringVar()
        self.tx2_power_summary = tk.StringVar()

        self.source_type = tk.StringVar(value="HEX")
        self.hex_data = tk.StringVar(value="AA55")
        self.bin_path = tk.StringVar()
        self.valid_bits = tk.StringVar(value="16")
        self.pn_seed = tk.StringVar(value="")
        self.pn_invert = tk.BooleanVar(value=False)
        self.frame_sync_enabled = tk.BooleanVar(value=False)
        self.frame_sync_word = tk.StringVar(value="FE6B2840")
        self.frame_sync_length = tk.StringVar(value="32")
        self.minor_frame_bytes = tk.StringVar(value="512")
        self.id_frame_count = tk.StringVar(value="1")
        self.sfid_start = tk.StringVar(value="0")
        self.frame_summary = tk.StringVar(
            value="小帧同步未启用；启用后按“同步字 + PN/数据载荷”组成固定长度小帧。")
        self.task_id = tk.StringVar(value="1")
        self.bit_channel = tk.StringVar(value="TX1")
        self.profile = tk.StringVar(value="RAW BIT / ILA验证")
        self.bit_rate = tk.StringVar(value="800000")
        self.bit_rate_mbps = tk.StringVar(value="0.8")
        self.pcm_deviation_hz = tk.StringVar(value="280000")
        self.pcm_deviation_mhz = tk.StringVar(value="0.28")
        self.pcm_frequency_offset_hz = tk.StringVar(value="0")
        self.pcm_frequency_offset_mhz = tk.StringVar(value="0")
        self.pcm_amplitude = tk.StringVar(value="0.80")
        self.line_code = tk.StringVar(value="NRZ-L")
        self.line_code_scope = tk.StringVar(value=line_code_ui_state(None)[1])
        self.rolloff = tk.StringVar(value="标准/无（PCM/FM Bessel）")
        self.soqpsk_differential = tk.BooleanVar(value=True)
        self.auto_preset = tk.BooleanVar(value=True)
        self.preset_summary = tk.StringVar(value="智能匹配：选择标准BIT调制模式后自动填入推荐参数；确认前不会下发。")
        # Telemetry is continuous by default.  A 16-bit single-shot frame at
        # 5 Mbit/s lasts only 3.2 us and is normally over before ILA is armed.
        self.repeat_mode = tk.StringVar(value="无限循环")
        self.repeat_count = tk.StringVar(value="0")
        self.gap_ms = tk.StringVar(value="2.0")
        self.bit_summary = tk.StringVar(value="预期：16 bit，MSB First，ILA序列 1010101001010101")
        self.bit_timing_summary = tk.StringVar(value="61.44 MSPS下：76.8 I/Q样点/BIT；单次时长20 us")
        self.progress = tk.DoubleVar(value=0)
        self.transfer_phase = tk.StringVar(value="待机：尚未下发BIT任务")

        self.ila_csv_path = tk.StringVar()
        self.ila_reference_path = tk.StringVar()
        self.ila_reference_bits = tk.StringVar()
        self.ila_line_code = tk.StringVar(value=ILA_LINE_CODE_CURRENT)

        self.monitor_source = tk.StringVar(value="TX DAC I/Q（推荐）")
        self.monitor_active_source = "TX DAC I/Q（推荐）"
        self.loopback_channel = tk.StringVar(value="通道1（TX1/RX1）")
        self.monitor_rx_lo_hz: int | None = None
        self.monitor_rx_bw_hz: int | None = None
        self.loopback_refresh_hz = tk.StringVar(value=str(MONITOR_REFRESH_HZ))
        self.loopback_status = tk.StringVar(
            value="默认抓取PL送往AD9361的TX DAC I/Q；可切换至外接SMA的真实RX ADC数据。")
        self.loopback_stats = tk.StringVar(value="尚无采集数据")
        self.monitor_stop = threading.Event()
        # Waveform/BIT control and monitor SNAP share the same board UDP
        # foreground service.  Pause capture traffic around every control
        # transaction so STOP/CLEAR/config/START cannot race a 26-packet frame.
        self.monitor_pause = threading.Event()
        self.monitor_quiesced = threading.Event()
        # At most one complete 32 KiB capture may wait for Tk/Matplotlib.
        # This is end-to-end backpressure: if rendering stalls, the worker
        # stops issuing SNAP instead of growing an unbounded event queue.
        self.monitor_frame_pending = threading.Event()
        self.monitor_thread: threading.Thread | None = None
        self.monitor_running = False
        self.monitor_render_count = 0
        self.monitor_first_render_time: float | None = None
        self.monitor_artists: dict[str, object] | None = None
        self.monitor_start_pending = False
        self.applied_waveform_signature: tuple[str, ...] | None = None

        self.events: queue.Queue[tuple[str, object]] = queue.Queue()
        self.worker_active = False
        self.network_busy = False
        self.config_busy = False
        self._board_sync_busy = False
        self._control_generation = 0
        self.config_status = tk.StringVar(value="当前配置：启动后自动只读查询板卡")
        self.transfer_thread: threading.Thread | None = None
        # Set when the operator applies a new waveform/RF configuration while
        # a BIT download is still sending.  The worker checks this between
        # packets so it cannot resurrect the task that the PS just aborted.
        self.transfer_cancel = threading.Event()
        self._unit_sync_busy = False

        self._configure_style()
        self._build()
        self._update_power_summaries()
        for variable in (self.valid_bits, self.hex_data, self.pn_seed,
                         self.frame_sync_word, self.frame_sync_length,
                         self.minor_frame_bytes, self.id_frame_count,
                         self.sfid_start):
            variable.trace_add("write", lambda *_args: self._update_summary())
        self.bit_rate.trace_add("write", lambda *_args: self._base_rate_changed())
        self.pcm_deviation_hz.trace_add(
            "write", lambda *_args: self._base_frequency_changed(
                self.pcm_deviation_hz, self.pcm_deviation_mhz))
        self.pcm_frequency_offset_hz.trace_add(
            "write", lambda *_args: self._base_frequency_changed(
                self.pcm_frequency_offset_hz, self.pcm_frequency_offset_mhz))
        self.bit_rate_mbps.trace_add("write", lambda *_args: self._mega_rate_changed())
        self.pcm_deviation_mhz.trace_add(
            "write", lambda *_args: self._mega_frequency_changed(
                self.pcm_deviation_mhz, self.pcm_deviation_hz, integer=False))
        self.pcm_frequency_offset_mhz.trace_add(
            "write", lambda *_args: self._mega_frequency_changed(
                self.pcm_frequency_offset_mhz,
                self.pcm_frequency_offset_hz, integer=False))
        self.tx_bw_mhz.trace_add("write", lambda *_args: self._update_rf_bandwidth_summary())
        self.after(80, self._poll_events)
        self.temperature_busy = False
        self.temperature_updated = 0.0
        self.after(2000, self._temperature_tick)
        self.after(350, self.refresh_board_configuration)
        self.protocol("WM_DELETE_WINDOW", self._close)

    def _configure_style(self) -> None:
        style = ttk.Style(self)
        style.theme_use("clam" if "clam" in style.theme_names() else style.theme_use())
        style.configure(".", font=("Microsoft YaHei UI", 10))
        style.configure("TFrame", background="#ffffff")
        style.configure("TLabel", background="#ffffff", foreground="#253248")
        style.configure("Title.TLabel", background="#ffffff", foreground="#10233f",
                        font=("Microsoft YaHei UI", 20, "bold"))
        style.configure("Sub.TLabel", background="#ffffff", foreground="#6b7a90")
        style.configure("Hint.TLabel", background="#ffffff", foreground="#64748b",
                        font=("Microsoft YaHei UI", 9))
        style.configure("Status.TLabel", background="#e8f1ff", foreground="#175cd3",
                        padding=(10, 5), font=("Microsoft YaHei UI", 9, "bold"))
        style.configure("TLabelframe", background="#ffffff", bordercolor="#d8e0eb",
                        lightcolor="#d8e0eb", darkcolor="#d8e0eb", relief="solid")
        style.configure("TLabelframe.Label", background="#ffffff", foreground="#21324b",
                        font=("Microsoft YaHei UI", 10, "bold"))
        style.configure("TNotebook", background="#ffffff", borderwidth=0)
        style.configure("TNotebook.Tab", padding=(20, 9), foreground="#526176",
                        font=("Microsoft YaHei UI", 10, "bold"))
        style.map("TNotebook.Tab",
                  background=[("selected", "#ffffff"), ("!selected", "#e8edf4")],
                  foreground=[("selected", "#175cd3"), ("!selected", "#526176")])
        style.configure("TButton", padding=(10, 7))
        style.layout("Choice.TRadiobutton", [
            ("Radiobutton.padding", {"sticky": "nswe", "children": [
                ("Radiobutton.label", {"sticky": "nswe"})]})])
        style.configure("Choice.TRadiobutton", padding=(9, 7), anchor="center",
                        background="#f0f4f9", foreground="#475569",
                        font=("Microsoft YaHei UI", 9))
        style.map("Choice.TRadiobutton",
                  background=[("disabled", "#f3f4f6"), ("selected", "#dbeafe"),
                              ("active", "#e2e8f0")],
                  foreground=[("disabled", "#9ca3af"), ("selected", "#1d4ed8")],
                  relief=[("focus", "solid"), ("!focus", "flat")])
        style.configure("Safe.TButton", background="#2563eb", foreground="#ffffff",
                        font=("Microsoft YaHei UI", 10, "bold"), borderwidth=0)
        style.map("Safe.TButton", background=[("active", "#1d4ed8"),
                                               ("disabled", "#9db7e5")])
        style.configure("Danger.TButton", background="#dc2626", foreground="#ffffff",
                        font=("Microsoft YaHei UI", 10, "bold"), borderwidth=0)
        style.map("Danger.TButton", background=[("active", "#b91c1c")])

    @staticmethod
    def _set_square(widget: tk.Label | None, color: str) -> None:
        """Change only a fixed status square's color, never its geometry."""
        if widget is not None:
            widget.configure(foreground=color)

    def _set_connection_status(self, state: str, text: str) -> None:
        colors = {"idle": "#94a3b8", "sent": "#d97706", "busy": "#2563eb",
                  "ok": "#16a34a", "error": "#dc2626"}
        variable = getattr(self, "connection_state", None)
        if variable is not None:
            variable.set(text)
        self._set_square(getattr(self, "connection_square", None),
                         colors.get(state, "#94a3b8"))

    def _set_operation_status(self, state: str, text: str) -> None:
        colors = {
            "idle": "#94a3b8", "busy": "#2563eb", "sent": "#d97706",
            "ok": "#16a34a",
            "error": "#dc2626", "stopped": "#d97706",
        }
        variable = getattr(self, "operation_state", None)
        if variable is not None:
            variable.set(text)
        self._set_square(getattr(self, "operation_square", None),
                         colors.get(state, "#94a3b8"))

    @staticmethod
    def _format_mega(base_value: str) -> str:
        return f"{float(base_value) / 1_000_000.0:.9g}"

    def _base_rate_changed(self) -> None:
        if not self._unit_sync_busy:
            try:
                formatted = self._format_mega(self.bit_rate.get())
            except ValueError:
                formatted = ""
            self._unit_sync_busy = True
            self.bit_rate_mbps.set(formatted)
            self._unit_sync_busy = False
        self._bit_rate_changed()

    def _base_frequency_changed(self, base: tk.StringVar,
                                display: tk.StringVar) -> None:
        if self._unit_sync_busy:
            return
        try:
            formatted = self._format_mega(base.get())
        except ValueError:
            formatted = ""
        self._unit_sync_busy = True
        display.set(formatted)
        self._unit_sync_busy = False

    def _mega_rate_changed(self) -> None:
        if self._unit_sync_busy:
            return
        try:
            value = float(self.bit_rate_mbps.get())
            if not math.isfinite(value):
                return
            base_value = str(int(round(value * 1_000_000.0)))
        except ValueError:
            return
        self._unit_sync_busy = True
        self.bit_rate.set(base_value)
        self._unit_sync_busy = False
        self._bit_rate_changed()

    def _mega_frequency_changed(self, display: tk.StringVar,
                                base: tk.StringVar, *, integer: bool) -> None:
        if self._unit_sync_busy:
            return
        try:
            value = float(display.get())
            if not math.isfinite(value):
                return
            scaled = value * 1_000_000.0
            base_value = str(int(round(scaled))) if integer else f"{scaled:.12g}"
        except ValueError:
            return
        self._unit_sync_busy = True
        base.set(base_value)
        self._unit_sync_busy = False

    def _commit_mega_fields(self, *, require_rate: bool = True,
                            require_deviation: bool = True,
                            require_offset: bool = True) -> None:
        """Validate used M-unit fields and update protocol base units."""
        def parse(variable: tk.StringVar, label: str) -> float:
            try:
                value = float(variable.get())
            except ValueError as exc:
                raise ValueError(f"{label}必须是有效数字") from exc
            if not math.isfinite(value):
                raise ValueError(f"{label}必须是有限数值")
            return value

        rate_mbps = parse(self.bit_rate_mbps, "码率") if require_rate else None
        deviation_mhz = (parse(self.pcm_deviation_mhz, "PCM/FM频偏")
                         if require_deviation else None)
        offset_mhz = (parse(self.pcm_frequency_offset_mhz, "基带偏移")
                      if require_offset else None)
        self._unit_sync_busy = True
        try:
            if rate_mbps is not None:
                self.bit_rate.set(str(int(round(rate_mbps * 1_000_000.0))))
            if deviation_mhz is not None:
                self.pcm_deviation_hz.set(f"{deviation_mhz * 1_000_000.0:.12g}")
            if offset_mhz is not None:
                self.pcm_frequency_offset_hz.set(f"{offset_mhz * 1_000_000.0:.12g}")
        finally:
            self._unit_sync_busy = False
        if rate_mbps is not None:
            self._bit_rate_changed()

    def _build(self) -> None:
        outer = ttk.Frame(self, padding=(18, 12))
        outer.pack(fill="both", expand=True)
        header = ttk.Frame(outer)
        header.pack(fill="x")
        title = ttk.Frame(header)
        title.pack(side="left", fill="x", expand=True)
        ttk.Label(title, text="AD9361TX 遥测信号发射", style="Title.TLabel").pack(anchor="w")
        ttk.Label(title, text=f"PCM/FM · SOQPSK-TG · PN与帧同步 · {UI_BUILD}",
                  style="Sub.TLabel").pack(anchor="w", pady=(2, 0))
        network = ttk.Frame(header)
        network.pack(side="right", anchor="ne", padx=(18, 0))
        self._build_network(network)

        self.book = ttk.Notebook(outer)
        self.book.pack(fill="both", expand=True, pady=(8, 8))
        rf_page = ttk.Frame(self.book)
        self.bit_page = ttk.Frame(self.book)
        monitor_tab = ttk.Frame(self.book, padding=12)
        self.book.add(rf_page, text="发射控制")
        self.book.add(self.bit_page, text="数据与帧")
        self.book.add(monitor_tab, text="TX/RX I/Q监控")
        rf_tab = self._scrollable_content(rf_page, padding=10)
        self.bit_tab = self._scrollable_content(self.bit_page, padding=12)
        self._build_rf_tab(rf_tab)
        self._build_monitor_tab(monitor_tab)
        self._build_bit_tab(self.bit_tab)
        self._build_log(outer)

    def _build_tfs_tab(self, tab: ttk.Frame) -> None:
        self.tfs_frequency = tk.StringVar(value="5000")
        self.tfs_fine = tk.BooleanVar(value=False)
        self.tfs_verify = tk.BooleanVar(value=False)
        # The TFS option-2 cold-start specification is 5 s; leave margin before
        # reporting a lock timeout immediately after powering the module.
        self.tfs_lock_timeout = tk.StringVar(value="6")
        self.tfs_attenuation = tk.StringVar(value="63")
        self.tfs_status = tk.StringVar(value="未查询；上电不自动向频综写入频率")
        self.tfs_active = False
        box = ttk.LabelFrame(tab, text="TFS21-22-25 · PL SPI控制", padding=16)
        box.pack(fill="x")
        ttk.Label(box, text="频综输出频率（MHz）").grid(row=0, column=0, sticky="w")
        ttk.Entry(box, textvariable=self.tfs_frequency, width=24).grid(row=0, column=1, padx=12)
        ttk.Label(box, text="50–22600 MHz").grid(row=0, column=2, sticky="w")
        ttk.Radiobutton(box, text="0.1 Hz步进", variable=self.tfs_fine, value=False).grid(row=1, column=0, pady=12)
        ttk.Radiobutton(box, text="0.1 mHz步进（型号支持时）", variable=self.tfs_fine, value=True).grid(row=1, column=1, columnspan=2, sticky="w")
        buttons = ttk.Frame(box)
        buttons.grid(row=2, column=0, columnspan=3, sticky="w")
        self.tfs_buttons = []
        for text, action in (("设置并检查锁定", "set"), ("读取频率", "read"), ("读取锁定", "lock"), ("查询SPI / LD", "status"), ("中止SPI", "abort")):
            button = ttk.Button(buttons, text=text, command=lambda a=action: self._tfs_action(a))
            button.pack(side="left", padx=(0, 12))
            self.tfs_buttons.append(button)
        ttk.Label(box, textvariable=self.tfs_status, wraplength=900).grid(row=3, column=0, columnspan=3, sticky="w", pady=14)
        ttk.Checkbutton(box, text="设置后核对寄存器读回（设备支持时启用）", variable=self.tfs_verify).grid(row=4, column=0, columnspan=2, sticky="w")
        wait_box = ttk.Frame(box)
        wait_box.grid(row=4, column=2, sticky="w")
        ttk.Label(wait_box, text="锁定超时(s，冷启动建议≥6)").pack(side="left")
        ttk.Entry(wait_box, textvariable=self.tfs_lock_timeout, width=6).pack(side="left")
        atten_box = ttk.Frame(box)
        atten_box.grid(row=5, column=0, columnspan=3, sticky="w", pady=(12, 4))
        ttk.Label(atten_box, text="外部衰减（dB）").pack(side="left")
        ttk.Spinbox(atten_box, from_=0, to=63, increment=.5, width=8,
                    textvariable=self.tfs_attenuation).pack(side="left", padx=8)
        for text, action in (("应用衰减", "atten_set"), ("读取设置", "atten_get"), ("最大衰减63 dB", "atten_max")):
            button = ttk.Button(atten_box, text=text, command=lambda a=action: self._tfs_action(a))
            button.pack(side="left", padx=(0, 8))
            self.tfs_buttons.append(button)
        ttk.Label(box, text="七线低有效 · 0–63 dB / 0.5 dB · PL配置后复位默认63 dB · 独立于AD9361内部衰减").grid(row=6, column=0, columnspan=3, sticky="w")
        ttk.Label(tab, text="独立PL SPI · 默认10 MHz · 3.3 V逻辑 · 外部时钟板提供100 MHz参考\n"
                  "J7：6=SCK，7=MOSI，8=LE，9=MISO，15=LD；另接公共地，TFS独立12 V供电。\n"
                  "频综本振与AD9361中心频率独立；LD表示锁定，输出频率仍需仪表核验。",
                  wraplength=950).pack(anchor="w", pady=(4, 8))

    def _tfs_action(self, action: str) -> None:
        if self.tfs_active:
            return
        try:
            board, local = self._endpoint()
            op, value = 0, 0
            fine = self.tfs_fine.get()
            verify = self.tfs_verify.get()
            lock_timeout = float(self.tfs_lock_timeout.get()) if action == "set" else 2.0
            if action == "set" and not .1 <= lock_timeout <= 30:
                raise ValueError("锁定超时范围为0.1–30秒；默认2秒是软件等待值")
            if action == "set":
                fine = self.tfs_fine.get()
                op = 1 if fine else 2
                value = frequency_units(self.tfs_frequency.get().strip(), fine)
            elif action == "abort":
                op = 3
            elif action == "atten_set":
                value = attenuation_units(self.tfs_attenuation.get().strip())
            elif action == "atten_max":
                self.tfs_attenuation.set("63")
        except ValueError as exc:
            self._error("频综参数错误", exc)
            return
        self.tfs_active = True
        self.tfs_status.set("正在处理；后台执行SPI和有界锁定检查")
        for button in self.tfs_buttons:
            button.configure(state="disabled")
        def worker() -> None:
            try:
                result = tfs_control_action(board, local, action, fine, value, verify, lock_timeout)
            except Exception as exc:
                result = f"失败：{exc}"
            self.events.put(("tfs_result", result))
        threading.Thread(target=worker, daemon=True).start()

    def _scrollable_content(self, page: ttk.Frame, *, padding: int) -> ttk.Frame:
        canvas = tk.Canvas(page, highlightthickness=0, background="#ffffff")
        scroll = ttk.Scrollbar(page, orient="vertical", command=canvas.yview)
        canvas.configure(yscrollcommand=scroll.set)
        canvas.pack(side="left", fill="both", expand=True)
        scroll.pack(side="right", fill="y")
        content = ttk.Frame(canvas, padding=padding)
        window = canvas.create_window((0, 0), window=content, anchor="nw")
        content.bind(
            "<Configure>",
            lambda _event: canvas.configure(scrollregion=canvas.bbox("all")))
        canvas.bind(
            "<Configure>",
            lambda event: canvas.itemconfigure(window, width=event.width))
        for widget in (canvas, content):
            widget.bind(
                "<MouseWheel>",
                lambda event, target=canvas: target.yview_scroll(
                    int(-event.delta / 120), "units"))
        return content

    def _build_network(self, parent: ttk.Frame) -> None:
        frame = ttk.Frame(parent)
        frame.pack(anchor="e")
        ttk.Label(frame, text="板卡（固定）").grid(row=0, column=0, sticky="w")
        ttk.Entry(frame, textvariable=self.board_ip, width=15, state="readonly").grid(
            row=0, column=1, padx=(6, 14))
        ttk.Label(frame, text="本机（自动获取）").grid(row=0, column=2, sticky="w")
        ttk.Entry(frame, textvariable=self.local_ip, width=15).grid(
            row=0, column=3, padx=(6, 10))
        self.network_test_button = ttk.Button(frame, text="测试", command=self.test_connection)
        self.network_test_button.grid(row=0, column=4)
        self.network_reset_button = ttk.Button(frame, text="重置连接", command=self.reset_connection)
        self.network_reset_button.grid(row=0, column=5, padx=(6, 0))
        ttk.Button(frame, text="刷新本机IP", command=self.refresh_local_address).grid(
            row=0, column=6, padx=(6, 0))
        ttk.Button(frame, text="读取板卡配置", command=self.refresh_board_configuration).grid(
            row=1, column=6, padx=(6, 0))
        status = ttk.Frame(frame)
        status.grid(row=1, column=0, columnspan=6, sticky="e", pady=(5, 0))
        self.connection_square = tk.Label(
            status, text="■", width=2, anchor="center", background="#ffffff",
            foreground="#94a3b8", font=("Segoe UI Symbol", 10))
        self.connection_square.pack(side="left")
        ttk.Label(status, textvariable=self.connection_state, style="Hint.TLabel").pack(
            side="left")
        ttk.Label(status, text="  UDP 40000  ·  61.44 MSPS / LVDS",
                  style="Hint.TLabel").pack(side="left")
        self.temperature_text = tk.StringVar(value="AD9361温度：等待读取")
        ttk.Label(frame, textvariable=self.temperature_text, style="Hint.TLabel").grid(
            row=2, column=0, columnspan=7, sticky="e", pady=(4, 0))
        ttk.Label(frame, textvariable=self.config_status, style="Hint.TLabel").grid(
            row=3, column=0, columnspan=7, sticky="e", pady=(4, 0))

    def _temperature_tick(self) -> None:
        # Tk access stays on the UI thread; only one short UDP read can be in flight.
        if time.monotonic() - self.temperature_updated > 6:
            self.temperature_text.set("AD9361温度：无新数据（需新版ELF）；Zynq温度未接入")
        if not (self.temperature_busy or self.worker_active or self.network_busy or self.config_busy or
                self.__dict__.get("tfs_active", False)):
            try:
                board, local = self._endpoint()
            except ValueError:
                pass
            else:
                self.temperature_busy = True
                def query() -> None:
                    try:
                        result = (read_chip_temperature(board, local), "")
                    except (OSError, ValueError) as exc:
                        result = (None, str(exc))
                    self.events.put(("temperature", result))
                threading.Thread(target=query, daemon=True).start()
        self.after(2000, self._temperature_tick)

    def refresh_local_address(self) -> None:
        if self.worker_active or self.network_busy or self.__dict__.get("tfs_active", False) or self.monitor_thread is not None and self.monitor_thread.is_alive():
            messagebox.showinfo("本机地址", "请先停止监控并等待当前控制操作完成。")
            return
        address = detect_local_ip(FIXED_BOARD_IP)
        self.local_ip.set(address)
        self._set_connection_status("idle", "本机地址已刷新，待测试" if address else "未获取本机地址，请检查网卡")
        self._log(f"本机路由地址：{address or '未获取'}；固定板卡 {FIXED_BOARD_IP}。自动获取不代表板卡已连接。")

    def _build_rf_tab(self, tab: ttk.Frame) -> None:
        common = ttk.LabelFrame(tab, text="射频与时钟", padding=12)
        common.pack(fill="x")
        ttk.Label(common, text="中心频率 (MHz)").grid(row=0, column=0, sticky="w")
        ttk.Entry(common, textvariable=self.tx_lo_mhz, width=14).grid(
            row=0, column=1, padx=(8, 24), sticky="w")
        ttk.Label(common, text="RF滤波带宽 (MHz)").grid(row=0, column=2, sticky="w")
        ttk.Entry(common, textvariable=self.tx_bw_mhz, width=12).grid(
            row=0, column=3, sticky="w", padx=(8, 24))
        ttk.Label(common, text="参考时钟").grid(row=0, column=4, sticky="w")
        ChoiceBar(common, variable=self.reference_source, values=tuple(REFERENCE_VALUE),
                  labels={"内部40 MHz晶振": "内部 40 MHz", "外部10 MHz参考": "外部 10 MHz"},
                  width=13).grid(row=0, column=5, sticky="w", padx=(8, 0))
        ttk.Button(common, text="应用时钟", command=self.apply_reference).grid(
            row=0, column=6, padx=(8, 0))
        ttk.Label(common, textvariable=self.rf_bw_summary, foreground="#52606d",
                  wraplength=1080).grid(
                      row=1, column=0, columnspan=8, sticky="w", pady=(8, 0))
        ttk.Label(
            common,
            textvariable=self.reference_status,
            style="Hint.TLabel", wraplength=1080).grid(
                row=2, column=0, columnspan=8, sticky="w", pady=(4, 0))
        common.columnconfigure(7, weight=1)

        self._build_tfs_tab(tab)

        channels = ttk.Frame(tab)
        channels.pack(fill="x", pady=(10, 0))
        tx1 = ttk.LabelFrame(channels, text=self.rf_channel_title.get(), padding=12)
        self.tx_channel_frame = tx1
        tx1.pack(side="left", fill="both", expand=True, padx=(0, 7))
        self._channel_controls(
            tx1, self.tx1_mode, self.tx1_power_dbm,
            self.tx1_zero_attenuation_dbm, self.tx1_power_summary)

        quick = ttk.LabelFrame(tab, text="常用调制与数据", padding=12)
        quick.pack(fill="x", pady=(10, 0))
        ttk.Label(quick, text="码率 (Mbit/s)").grid(row=0, column=0, sticky="w")
        ttk.Entry(quick, textvariable=self.bit_rate_mbps, width=13).grid(
            row=0, column=1, padx=(8, 22), sticky="w")
        ttk.Label(quick, text="数据源").grid(row=0, column=2, sticky="w")
        ttk.Label(quick, textvariable=self.source_type,
                  style="Hint.TLabel").grid(row=0, column=3, padx=(8, 22), sticky="w")
        ttk.Label(quick, text="有效BIT数").grid(row=0, column=4, sticky="w")
        self.quick_valid_bits_entry = ttk.Entry(quick, textvariable=self.valid_bits, width=12)
        self.quick_valid_bits_entry.grid(
            row=0, column=5, padx=(8, 22), sticky="w")
        ttk.Checkbutton(
            quick, text="加入帧同步", variable=self.frame_sync_enabled,
            command=self._frame_sync_changed).grid(row=0, column=6, columnspan=2, sticky="w")

        ttk.Label(quick, text="常用数据源").grid(row=1, column=0, sticky="w", pady=(6, 0))
        ChoiceBar(quick, variable=self.source_type,
                  values=("HEX", "BIN文件", "PN11", "PN15", "PRBS15"),
                  labels={"BIN文件": "BIN"}, command=self._source_changed,
                  width=8).grid(row=1, column=1, columnspan=6,
                                padx=(8, 0), pady=(6, 0), sticky="w")
        ttk.Button(quick, text="更多数据源…",
                   command=lambda: self.book.select(self.bit_page)).grid(
                       row=1, column=7, pady=(6, 0), sticky="e")

        ttk.Label(quick, text="PCM频偏 (MHz)").grid(row=2, column=0, sticky="w", pady=(9, 0))
        ttk.Entry(quick, textvariable=self.pcm_deviation_mhz, width=13).grid(
            row=2, column=1, padx=(8, 22), pady=(9, 0), sticky="w")
        ttk.Label(quick, text="PCM码型").grid(row=3, column=0, sticky="w", pady=(6, 0))
        self.quick_line_code_box = ChoiceBar(
            quick, variable=self.line_code, values=tuple(LINE_CODE_VALUE),
            labels={"Biφ-L（Manchester）": "Biφ-L", "RNRZ-L（IRIG随机化）": "RNRZ-L"},
            command=self._update_summary, state="disabled", width=8)
        self.quick_line_code_box.grid(
            row=3, column=1, columnspan=7, padx=(8, 0), pady=(6, 0), sticky="w")
        ttk.Label(quick, text="重复方式").grid(row=2, column=2, sticky="w", pady=(9, 0))
        ChoiceBar(quick, variable=self.repeat_mode,
                  values=("单次", "指定附加次数", "无限循环"),
                  labels={"指定附加次数": "指定次数"},
                  command=self._repeat_changed, width=8).grid(
                      row=2, column=3, columnspan=3, sticky="w", padx=(8, 22), pady=(9, 0))
        ttk.Label(quick, text="附加次数").grid(row=2, column=6, sticky="w", pady=(9, 0))
        self.repeat_entry = ttk.Entry(
            quick, textvariable=self.repeat_count, width=12, state="disabled")
        self.repeat_entry.grid(row=2, column=7, padx=(8, 0), pady=(9, 0), sticky="ew")
        ttk.Label(quick, textvariable=self.preset_summary, foreground="#047857",
                  wraplength=1050).grid(
                      row=4, column=0, columnspan=8, sticky="w", pady=(8, 0))
        ttk.Label(quick, textvariable=self.line_code_scope, style="Hint.TLabel",
                  wraplength=1080).grid(row=5, column=0, columnspan=8, sticky="w", pady=(3, 0))
        ttk.Label(
            quick,
            text=("实现上限：PCM/FM、SOQPSK-TG为20 Mbit/s（61.44 MSPS，积分相位响应；需配套新版PL/PS）。"
                  "B99不是可控参数，需由实际码流、频偏/成形和频谱仪测量确定；"
                  "RF滤波带宽仅为AD9361模拟滤波器设置。"),
            style="Hint.TLabel", wraplength=1080).grid(
                row=6, column=0, columnspan=8, sticky="w", pady=(4, 0))
        quick.columnconfigure(7, weight=1)

        buttons = ttk.Frame(tab)
        buttons.pack(fill="x", pady=(10, 0))
        self.send_button = ttk.Button(
            buttons, text="应用并开始发射", command=self.apply_tx,
            style="Safe.TButton")
        self.send_button.pack(side="left", fill="x", expand=True, padx=(0, 6))
        ttk.Button(buttons, text="查询BIT/RF状态", command=self.query_tx_runtime_status).pack(
            side="left", fill="x", expand=True, padx=6)
        ttk.Button(buttons, text="紧急关闭双TX", command=self.emergency_off,
                   style="Danger.TButton").pack(side="left", fill="x", expand=True, padx=(6, 0))
        operation = ttk.Frame(tab)
        operation.pack(fill="x", pady=(6, 0))
        self.operation_square = tk.Label(
            operation, text="■", width=2, anchor="center", background="#ffffff",
            foreground="#94a3b8", font=("Segoe UI Symbol", 10))
        self.operation_square.pack(side="left")
        ttk.Label(operation, textvariable=self.operation_state,
                  style="Hint.TLabel").pack(side="left")
        ttk.Label(operation, text="  ").pack(side="left")
        ttk.Label(operation, textvariable=self.transfer_phase,
                  style="Hint.TLabel").pack(side="left")

    def _channel_controls(self, parent: ttk.LabelFrame, mode: tk.StringVar,
                          power: tk.StringVar, zero_attenuation_power: tk.StringVar,
                          summary: tk.StringVar) -> None:
        ChoiceBar(parent, variable=mode, values=VISIBLE_MODE_VALUES,
                  labels={"PCM/FM（PL）": "PCM/FM", "SOQPSK-TG（PL）": "SOQPSK-TG",
                          "正弦波/CW（PL）": "正弦 / CW"},
                  command=self._mode_changed, width=9).grid(
                      row=0, column=0, columnspan=2, sticky="ew")
        ttk.Label(parent, text="目标输出功率 (dBm)").grid(
            row=1, column=0, sticky="w", pady=(12, 0))
        ttk.Entry(parent, textvariable=power, width=18).grid(
            row=1, column=1, padx=(10, 0), pady=(12, 0), sticky="ew")
        ttk.Label(parent, text="0 dB衰减实测功率 (dBm)").grid(
            row=2, column=0, sticky="w", pady=(10, 0))
        ttk.Entry(parent, textvariable=zero_attenuation_power, width=18).grid(
            row=2, column=1, padx=(10, 0), pady=(10, 0), sticky="ew")
        ttk.Label(parent, textvariable=summary, foreground="#52606d", wraplength=390).grid(
            row=3, column=0, columnspan=2, sticky="w", pady=(6, 0))
        power.trace_add("write", lambda *_args: self._update_power_summaries())
        zero_attenuation_power.trace_add(
            "write", lambda *_args: self._update_power_summaries())
        parent.columnconfigure(1, weight=1)

    def _build_bit_tab(self, tab: ttk.Frame) -> None:
        source = ttk.LabelFrame(tab, text="BIT内容", padding=12)
        source.pack(fill="x")
        ttk.Label(source, text="数据源").grid(row=0, column=0, sticky="w")
        ChoiceBar(source, variable=self.source_type, values=SOURCE_VALUES,
                  columns=6, width=7, command=self._source_changed).grid(
                      row=0, column=1, padx=(8, 22), sticky="w")
        ttk.Label(source, text="有效BIT数").grid(row=0, column=2, sticky="w")
        self.valid_bits_entry = ttk.Entry(source, textvariable=self.valid_bits, width=15)
        self.valid_bits_entry.grid(row=0, column=3, padx=(8, 0), sticky="w")
        ttk.Label(source, text="HEX数据").grid(row=1, column=0, sticky="w", pady=(10, 0))
        self.hex_entry = ttk.Entry(source, textvariable=self.hex_data)
        self.hex_entry.grid(row=1, column=1, columnspan=3, sticky="ew",
                            padx=(8, 0), pady=(10, 0))
        ttk.Label(source, text="BIN文件").grid(row=2, column=0, sticky="w", pady=(10, 0))
        self.bin_entry = ttk.Entry(source, textvariable=self.bin_path, state="disabled")
        self.bin_entry.grid(row=2, column=1, columnspan=2, sticky="ew",
                            padx=(8, 8), pady=(10, 0))
        self.bin_button = ttk.Button(source, text="浏览…", command=self.choose_bin,
                                     state="disabled")
        self.bin_button.grid(row=2, column=3, pady=(10, 0), sticky="ew")
        ttk.Label(source, text="PN初始值 (HEX)").grid(
            row=3, column=0, sticky="w", pady=(10, 0))
        self.pn_seed_entry = ttk.Entry(source, textvariable=self.pn_seed,
                                       width=15, state="disabled")
        self.pn_seed_entry.grid(row=3, column=1, padx=(8, 22), pady=(10, 0), sticky="w")
        self.pn_invert_button = ttk.Checkbutton(
            source, text="PN输出反相", variable=self.pn_invert,
            command=self._update_summary, state="disabled")
        self.pn_invert_button.grid(row=3, column=2, columnspan=2,
                                   sticky="w", pady=(10, 0))
        source.columnconfigure(1, weight=1)
        source.columnconfigure(3, weight=1)

        framing = ttk.LabelFrame(tab, text="可选PCM小帧同步", padding=12)
        framing.pack(fill="x", pady=(12, 0))
        ttk.Checkbutton(
            framing, text="加入IRIG-106 PCM小帧同步",
            variable=self.frame_sync_enabled,
            command=self._frame_sync_changed).grid(
                row=0, column=0, columnspan=2, sticky="w")
        ttk.Label(framing, text="同步字长度 (16–33 bit)").grid(
            row=1, column=0, sticky="w", pady=(10, 0))
        self.frame_sync_length_entry = ttk.Entry(
            framing, textvariable=self.frame_sync_length, width=18, state="disabled")
        self.frame_sync_length_entry.grid(
            row=1, column=1, padx=(8, 22), pady=(10, 0), sticky="w")
        ttk.Label(framing, text="同步字 (HEX)").grid(
            row=1, column=2, sticky="w", pady=(10, 0))
        self.frame_sync_word_entry = ttk.Entry(
            framing, textvariable=self.frame_sync_word, width=18, state="disabled")
        self.frame_sync_word_entry.grid(
            row=1, column=3, padx=(8, 0), pady=(10, 0), sticky="ew")
        ttk.Label(framing, text="小帧长度 (byte)").grid(
            row=2, column=0, sticky="w", pady=(10, 0))
        self.minor_frame_bytes_entry = ttk.Entry(
            framing, textvariable=self.minor_frame_bytes, width=18, state="disabled")
        self.minor_frame_bytes_entry.grid(
            row=2, column=1, padx=(8, 22), pady=(10, 0), sticky="w")
        ttk.Label(framing, text="接收机示例：32 bit / FE6B2840 / 512 byte",
                  style="Hint.TLabel").grid(
                      row=2, column=2, columnspan=2, sticky="w", pady=(10, 0))
        ttk.Label(framing, textvariable=self.frame_summary, foreground="#047857",
                  wraplength=900).grid(
                      row=5, column=0, columnspan=4, sticky="w", pady=(10, 0))
        ttk.Label(framing, text="帧类型/小帧数 (1=无副帧)").grid(
            row=3, column=0, sticky="w", pady=(10, 0))
        self.id_frame_count_entry = ttk.Spinbox(
            framing, from_=1, to=256, textvariable=self.id_frame_count,
            width=18, state="disabled")
        self.id_frame_count_entry.grid(row=3, column=1, padx=(8, 22),
                                       pady=(10, 0), sticky="w")
        ttk.Label(framing, text="SFID：同步字后8 bit，递增；大帧≤16 KiB",
                  style="Hint.TLabel").grid(row=3, column=2, columnspan=2, sticky="w")
        ttk.Label(framing, text="SFID起始值 (0/1)").grid(
            row=4, column=0, sticky="w", pady=(10, 0))
        self.sfid_start_entry = ttk.Spinbox(
            framing, from_=0, to=1, textvariable=self.sfid_start,
            width=18, state="disabled")
        self.sfid_start_entry.grid(row=4, column=1, padx=(8, 22),
                                   pady=(10, 0), sticky="w")
        ttk.Label(framing, text="接收端须使用相同起始值；256小帧仅支持从0开始",
                  style="Hint.TLabel").grid(row=4, column=2, columnspan=2, sticky="w")
        ttk.Label(
            framing,
            text="同步掩码、容错BIT、CTL和LTS属于接收端捕获参数，不在发射端配置。",
            style="Hint.TLabel", wraplength=900).grid(
                row=6, column=0, columnspan=4, sticky="w", pady=(5, 0))
        framing.columnconfigure(3, weight=1)

        summary = ttk.LabelFrame(tab, text="发送数据核对", padding=12)
        summary.pack(fill="x", pady=(12, 0))
        ttk.Label(summary, textvariable=self.bit_summary, foreground="#1d4ed8",
                  wraplength=1000).pack(anchor="w")
        ttk.Label(summary, textvariable=self.bit_timing_summary, foreground="#047857",
                  wraplength=1000).pack(anchor="w", pady=(6, 0))
        ttk.Label(
            summary,
            text="此处显示本次将下载的BIT预期值；板端接收需以BIT状态确认。启用帧同步时，有效BIT数由小帧长度决定。",
            style="Hint.TLabel").pack(anchor="w", pady=(6, 0))

    def _build_ila_tab(self, tab: ttk.Frame) -> None:
        source = ttk.LabelFrame(tab, text="ILA抓取文件与参考BIT", padding=12)
        source.pack(fill="x")
        ttk.Label(source, text="ILA CSV").grid(row=0, column=0, sticky="w")
        ttk.Entry(source, textvariable=self.ila_csv_path).grid(
            row=0, column=1, sticky="ew", padx=(8, 8))
        ttk.Button(source, text="选择CSV…", command=self.choose_ila_csv).grid(
            row=0, column=2, sticky="ew")

        ttk.Label(source, text="参考BIN（可选）").grid(row=1, column=0, sticky="w", pady=(10, 0))
        ttk.Entry(source, textvariable=self.ila_reference_path).grid(
            row=1, column=1, sticky="ew", padx=(8, 8), pady=(10, 0))
        ttk.Button(source, text="选择参考BIN…", command=self.choose_ila_reference).grid(
            row=1, column=2, sticky="ew", pady=(10, 0))

        ttk.Label(source, text="参考有效BIT数").grid(row=2, column=0, sticky="w", pady=(10, 0))
        ttk.Entry(source, textvariable=self.ila_reference_bits, width=18).grid(
            row=2, column=1, sticky="w", padx=(8, 8), pady=(10, 0))
        ttk.Button(source, text="改用当前发送页数据源",
                   command=self.use_current_ila_reference).grid(
                       row=2, column=2, sticky="ew", pady=(10, 0))

        ttk.Label(source, text="抓取时PCM码型").grid(row=3, column=0, sticky="w", pady=(10, 0))
        ttk.Combobox(
            source, textvariable=self.ila_line_code,
            values=(ILA_LINE_CODE_CURRENT, *LINE_CODE_VALUE.keys()),
            state="readonly", width=26).grid(
                row=3, column=1, sticky="w", padx=(8, 8), pady=(10, 0))
        ttk.Label(
            source,
            text=("参考BIN留空时，BER自动使用“指定BIT发送”页当前的HEX/BIN/PRBS数据；"
                  "BIT率目标也读取该页当前BIT率。码型判断需要CSV包含pcm_debug_nrz，"
                  "推荐同时包含bit_phase。"),
            foreground="#52606d", wraplength=810).grid(
                row=4, column=0, columnspan=3, sticky="w", pady=(10, 0))
        source.columnconfigure(1, weight=1)

        controls = ttk.Frame(tab)
        controls.pack(fill="x", pady=(12, 0))
        ttk.Button(controls, text="开始计算BIT率、BER与码型",
                   command=self.analyze_ila_capture, style="Safe.TButton").pack(
                       side="left", fill="x", expand=True, padx=(0, 6))
        ttk.Button(controls, text="清空结果", command=lambda: self._set_ila_result("")).pack(
            side="left", fill="x", expand=True, padx=(6, 0))

        result = ttk.LabelFrame(tab, text="分析结果", padding=8)
        result.pack(fill="both", expand=True, pady=(12, 0))
        self.ila_result_widget = tk.Text(
            result, height=25, wrap="word", state="disabled",
            font=("Consolas", 10), background="#f8fafc", foreground="#0f172a")
        scroll = ttk.Scrollbar(result, orient="vertical", command=self.ila_result_widget.yview)
        self.ila_result_widget.configure(yscrollcommand=scroll.set)
        self.ila_result_widget.pack(side="left", fill="both", expand=True)
        scroll.pack(side="right", fill="y")
        self._set_ila_result(
            "请选择任意路径的Vivado ILA CSV。至少需要bit_valid、bit_data；\n"
            "码型判断还需要pcm_debug_nrz，精确分半建议包含bit_phase，BER建议包含bits_sent。")

    def _build_monitor_tab(self, tab: ttk.Frame) -> None:
        controls = ttk.LabelFrame(tab, text="TX/RX I/Q 实时监视", padding=8)
        controls.pack(fill="x", pady=(10, 0))
        ttk.Label(controls, text="通道").grid(row=0, column=0, sticky="w")
        ttk.Label(controls, textvariable=self.monitor_channel_title).grid(
            row=0, column=1, padx=(8, 16), sticky="w")
        ttk.Label(controls, text="数据源").grid(row=0, column=2, sticky="w")
        ChoiceBar(
            controls, variable=self.monitor_source,
            values=tuple(MONITOR_SOURCE_VALUE), width=14,
            labels={"TX DAC I/Q（推荐）": "TX DAC", "RX ADC·外接SMA自收": "RX ADC·SMA"}).grid(
                row=0, column=3, padx=(8, 12), sticky="w")
        self.monitor_start_button = ttk.Button(
            controls, text="开始监视", command=self.start_loopback_monitor,
            style="Safe.TButton")
        self.monitor_start_button.grid(row=0, column=4, padx=(8, 5), sticky="e")
        self.monitor_stop_button = ttk.Button(
            controls, text="停止监视", command=self.stop_loopback_monitor,
            state="disabled")
        self.monitor_stop_button.grid(row=0, column=5, padx=(5, 0), sticky="e")
        ttk.Label(
            controls,
            textvariable=self.monitor_route_hint,
            style="Hint.TLabel").grid(row=1, column=0, columnspan=6, sticky="w", pady=(6, 0))
        ttk.Label(
            controls, textvariable=self.loopback_status, foreground="#175cd3",
            wraplength=1050).grid(row=2, column=0, columnspan=6, sticky="w", pady=(6, 0))
        ttk.Label(
            controls, textvariable=self.loopback_stats, foreground="#047857",
            wraplength=1050).grid(row=3, column=0, columnspan=6, sticky="w", pady=(3, 0))
        self.monitor_plot_note = tk.StringVar(value="显示原始接收/发射I/Q时域与复基带频谱，不执行解调。")
        ttk.Label(
            controls, textvariable=self.monitor_plot_note,
            style="Hint.TLabel", wraplength=1050).grid(
                row=4, column=0, columnspan=6, sticky="w", pady=(3, 0))
        controls.columnconfigure(4, weight=1)

        plot_frame = ttk.LabelFrame(tab, text="I/Q时域与复基带频谱", padding=3)
        plot_frame.pack(fill="both", expand=True, pady=(6, 0))
        if Figure is None or FigureCanvasTkAgg is None or np is None:
            ttk.Label(
                plot_frame,
                text="缺少NumPy或Matplotlib，无法绘图。请在当前Python环境安装numpy、matplotlib。",
                foreground="#dc2626").pack(padx=20, pady=30)
            self.monitor_figure = None
            self.monitor_canvas = None
            self.monitor_start_button.configure(state="disabled")
            return

        self.monitor_figure = Figure(figsize=(10.5, 2.4), dpi=100, facecolor="#ffffff")
        self.monitor_axes = self.monitor_figure.subplots(1, 2)
        self.monitor_figure.subplots_adjust(left=0.055, right=0.985, bottom=0.20,
                                            top=0.88, wspace=0.28)
        self.monitor_canvas = FigureCanvasTkAgg(self.monitor_figure, master=plot_frame)
        self.monitor_canvas.get_tk_widget().pack(fill="both", expand=True)
        self._draw_empty_monitor()

    def _draw_empty_monitor(self) -> None:
        if self.monitor_figure is None:
            return
        self.monitor_artists = None
        titles = ("I/Q时域（0–10 µs）", "复基带频谱")
        for axis, title in zip(self.monitor_axes, titles):
            axis.clear()
            axis.set_title(title)
            axis.grid(True, alpha=0.25)
            axis.text(0.5, 0.5, "等待完整UDP采集帧", ha="center", va="center",
                      transform=axis.transAxes, color="#64748b")
        self.monitor_canvas.draw_idle()

    def _initialize_monitor_artists(self) -> None:
        """Create Matplotlib artists once; later frames only replace data."""
        axis_time, axis_spectrum = self.monitor_axes
        for axis in self.monitor_axes:
            axis.clear()
            axis.grid(True, alpha=0.25)

        line_i, = axis_time.plot([], [], label="I", linewidth=0.8)
        line_q, = axis_time.plot([], [], label="Q", linewidth=0.8)
        axis_time.set_xlabel("时间 (µs)")
        axis_time.set_ylabel("归一化幅度")
        axis_time.set_xlim(0.0, MONITOR_TIME_WINDOW_US)
        axis_time.set_ylim(-1.05, 1.05)
        axis_time.legend(loc="upper right", fontsize=8)

        spectrum_line, = axis_spectrum.plot([], [], linewidth=0.8,
                                             color="#2563eb")
        axis_spectrum.set_xlabel("相对本振的频偏 (MHz)")
        axis_spectrum.set_ylabel("幅度 (dBFS，Hann窗)")
        axis_spectrum.set_xlim(-IQ_SAMPLE_RATE_HZ / 2e6,
                               IQ_SAMPLE_RATE_HZ / 2e6)
        axis_spectrum.set_ylim(-120, 5)
        spectrum_peaks = axis_spectrum.scatter(
            [], [], s=12, color="#d97706", zorder=3)
        spectrum_note = axis_spectrum.text(
            0.02, 0.97, "", va="top", fontsize=8,
            transform=axis_spectrum.transAxes, color="#92400e",
            bbox={"facecolor": "white", "alpha": 0.8, "edgecolor": "none"})

        self.monitor_artists = {
            "time_i": line_i,
            "time_q": line_q,
            "spectrum": spectrum_line,
            "spectrum_peaks": spectrum_peaks,
            "spectrum_note": spectrum_note,
        }

    def start_loopback_monitor(self) -> None:
        if self.__dict__.get("network_busy", False):
            self._log("正在检测/重置连接，请完成后再启动监视。")
            return
        if self.monitor_running:
            return
        if np is None or self.monitor_canvas is None:
            self._error("绘图环境不可用", "需要NumPy和Matplotlib。")
            return
        try:
            board, local = self._endpoint()
            refresh_hz = MONITOR_REFRESH_HZ
            monitor_source = self.monitor_source.get()
            if monitor_source not in MONITOR_SOURCE_VALUE:
                raise ValueError("请选择有效的实时监视数据源")
            self.loopback_refresh_hz.set(str(refresh_hz))
        except ValueError as exc:
            self._error("监视参数错误", exc)
            return
        if self.tx1_mode.get() == "不发射":
            messagebox.showwarning(
                "尚未选择发射波形",
                "当前发射通道未开启。请先选择并应用一个波形，否则监视数据只能是静止值。",
                parent=self)
            return
        if monitor_source == "RX ADC·外接SMA自收":
            if not messagebox.askyesno(
                    "确认外接射频自收安全",
                    "此模式会把RX本振自动设为TX本振，并从外部SMA接收真实射频。\n\n"
                    "请确认对应通道已通过至少30 dB固定50 Ω衰减器连接："
                    f"TX{self._rf_ports()[1]}A→RX{self._rf_ports()[0]}A。禁止TX/RX口直连；"
                    "开始后会按当前波形开启发射。确认接线和衰减器已就绪？",
                    parent=self):
                return
        signature = self._current_waveform_signature()
        if self.applied_waveform_signature != signature:
            if not messagebox.askyesno(
                    "先启动当前发射波形",
                    "当前界面参数尚未完成本次发射事务，DMA只能抓到静止数据。\n\n"
                    "是否现在自动执行“BIT下载/模式配置/START”，成功后再开始监视？",
                    parent=self):
                return
            self.monitor_start_pending = True
            self.apply_tx()
            if not self.worker_active and self.applied_waveform_signature != signature:
                self.monitor_start_pending = False
            return

        self.monitor_stop.clear()
        self.monitor_pause.clear()
        self.monitor_quiesced.clear()
        self.monitor_frame_pending.clear()
        self.monitor_running = True
        self.monitor_render_count = 0
        self.monitor_first_render_time = None
        self.monitor_start_button.configure(state="disabled")
        self.monitor_stop_button.configure(state="normal")
        self.monitor_active_source = monitor_source
        self.loopback_status.set(
            f"正在选择{monitor_source}并绑定{local}:{DATA_PORT}；目标{refresh_hz} frame/s。")
        self.loopback_stats.set("等待板卡ARP解析和第一帧DMA数据……")
        self.monitor_thread = threading.Thread(
            target=self._loopback_monitor_worker,
            args=(board, local, refresh_hz, monitor_source), daemon=True)
        self.monitor_thread.start()

    def stop_loopback_monitor(self) -> None:
        self.monitor_stop.set()
        self.monitor_pause.clear()
        self.monitor_quiesced.clear()
        self.monitor_frame_pending.clear()
        self.loopback_status.set("正在停止I/Q采集并恢复RX默认配置……")
        try:
            with self._new_socket() as sock:
                sock.sendto(b"STR0", (self.board_ip.get().strip(), CTRL_PORT))
                sock.sendto(loopback_control_packet(False),
                            (self.board_ip.get().strip(), CTRL_PORT))
                if self.monitor_active_source == "RX ADC·外接SMA自收":
                    sock.sendto(rf_rx_control_packet(False),
                                (self.board_ip.get().strip(), CTRL_PORT))
                sock.sendto(monitor_source_packet(1),
                            (self.board_ip.get().strip(), CTRL_PORT))
        except (OSError, ValueError) as exc:
            self._log(f"关闭回环命令发送失败：{exc}")

    def _loopback_monitor_worker(self, board: str, local: str,
                                 refresh_hz: int, monitor_source: str) -> None:
        interval = 1.0 / refresh_hz
        requested = completed = incomplete = invalid = packet_count = 0
        wire_bytes = 0
        frames: dict[int, ZsdrDatagram] = {}
        active_capture: int | None = None
        awaiting = False
        deadline = 0.0
        next_request = time.monotonic()
        first_complete_time: float | None = None
        pause_active = False

        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                # Two UI instances must not silently share the IQ receive port.
                if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
                    sock.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
                sock.bind((local, DATA_PORT))
                sock.settimeout(0.025)
                # Stop the legacy unpaced continuous uploader.  Display capture
                # uses one SNAP transaction at a time so the 32-BD TX ring cannot
                # be overrun by a second 26-packet capture.
                sock.sendto(b"STR0", (board, CTRL_PORT))
                time.sleep(0.020)
                source, enable_loopback = MONITOR_SOURCE_VALUE[monitor_source]
                # Leave AD9361 loopback first, then switch the idle PL packer.
                sock.sendto(loopback_control_packet(False), (board, CTRL_PORT))
                if monitor_source == "RX ADC·外接SMA自收":
                    sock.sendto(rf_rx_control_packet(True), (board, CTRL_PORT))
                    time.sleep(0.080)  # allow RX synthesizer/profile readback to settle
                    rx_lo_hz = read_board_u32(board, local, b"RXFQ", b"RXFA") * 1000
                    rx_bw_hz = read_board_u32(board, local, b"RXBW", b"RXBA")
                    if not 70_000_000 <= rx_lo_hz <= 6_000_000_000 or not 200_000 <= rx_bw_hz <= 56_000_000:
                        raise ValueError("板端RX中心频率/滤波带宽读回超出AD9361范围")
                else:
                    rx_lo_hz = rx_bw_hz = None
                sock.sendto(monitor_source_packet(source), (board, CTRL_PORT))
                if enable_loopback:
                    sock.sendto(loopback_control_packet(True), (board, CTRL_PORT))
                time.sleep(0.050)
                # The loopback API's software cache is not sufficient proof.
                # TXST makes the firmware print the physical OBSERVE_CONFIG
                # loop-test bit, both sample rates and both LOs to the console.
                sock.sendto(b"TXST", (board, CTRL_PORT))
                time.sleep(0.020)
                self.events.put(("monitor_started", {
                    "refresh_hz": refresh_hz,
                    "source": monitor_source,
                    "loopback": enable_loopback,
                    "rx_lo_hz": rx_lo_hz,
                    "rx_bw_hz": rx_bw_hz,
                }))

                while not self.monitor_stop.is_set():
                    now = time.monotonic()
                    if self.monitor_pause.is_set():
                        if not pause_active:
                            # STR0 stops any legacy stream.  A SNAP already in
                            # progress is finite; discard its partial PC-side
                            # assembly and drain late packets while the control
                            # worker waits a short settling interval.
                            sock.sendto(b"STR0", (board, CTRL_PORT))
                            frames = {}
                            active_capture = None
                            awaiting = False
                            pause_active = True
                            self.monitor_quiesced.set()
                        try:
                            sock.recvfrom(2048)  # drain the tail of an old capture
                        except socket.timeout:
                            pass
                        continue
                    if pause_active:
                        pause_active = False
                        self.monitor_quiesced.clear()
                        next_request = now + max(interval, 0.050)

                    if (not awaiting and not self.monitor_frame_pending.is_set()
                            and now >= next_request):
                        # Default stays SNAP for the original 7020 firmware.
                        # P351 may negotiate a short, fixed-size monitor block.
                        sock.sendto(self.__dict__.get("monitor_capture_request", b"SNAP"),
                                    (board, CTRL_PORT))
                        requested += 1
                        awaiting = True
                        deadline = now + max(0.75, interval * 4.0)
                        next_request = now + interval

                    try:
                        datagram, endpoint = sock.recvfrom(2048)
                    except socket.timeout:
                        datagram = b""
                        endpoint = ("", 0)
                    if datagram and endpoint[0] == board:
                        try:
                            frame = parse_zsdr_datagram(datagram)
                        except ValueError:
                            invalid += 1
                        else:
                            packet_count += 1
                            wire_bytes += len(datagram) + 42
                            if active_capture != frame.capture_id:
                                if frames:
                                    incomplete += 1
                                active_capture = frame.capture_id
                                frames = {}
                            frames[frame.frame_index] = frame
                            if len(frames) == frame.frame_count:
                                try:
                                    payload = assemble_zsdr_capture(frames)
                                except ValueError:
                                    # A damaged or mixed capture is a dropped
                                    # frame, not a reason to stop monitoring.
                                    invalid += 1
                                    frames = {}
                                    active_capture = None
                                    awaiting = False
                                    next_request = time.monotonic()
                                    continue
                                completed += 1
                                complete_time = time.monotonic()
                                if first_complete_time is None:
                                    first_complete_time = complete_time
                                elapsed = max(complete_time - first_complete_time, 1e-6)
                                actual_fps = 1.0 if completed == 1 else (completed - 1) / elapsed
                                network_mbps = wire_bytes * 8.0 / max(
                                    complete_time - (first_complete_time or complete_time),
                                    interval) / 1e6
                                self.monitor_frame_pending.set()
                                self.events.put(("monitor_frame", {
                                    "capture_id": frame.capture_id,
                                    "timestamp": frame.timestamp,
                                    "payload": payload,
                                    "requested": requested,
                                    "completed": completed,
                                    "incomplete": incomplete,
                                    "invalid": invalid,
                                    "packets": packet_count,
                                    "actual_fps": actual_fps,
                                    "network_mbps": network_mbps,
                                }))
                                frames = {}
                                active_capture = None
                                awaiting = False
                                next_request = max(next_request, complete_time)

                    now = time.monotonic()
                    if awaiting and now >= deadline:
                        incomplete += 1
                        timeout_detail = {
                            "incomplete": incomplete, "received": len(frames),
                            "expected": next(iter(frames.values())).frame_count if frames else 0,
                            "packets": packet_count, "invalid": invalid,
                            "requested": requested, "completed": completed,
                        }
                        frames = {}
                        active_capture = None
                        awaiting = False
                        next_request = now
                        self.events.put(("monitor_timeout", timeout_detail))

                sock.sendto(b"STR0", (board, CTRL_PORT))
                sock.sendto(loopback_control_packet(False), (board, CTRL_PORT))
                if monitor_source == "RX ADC·外接SMA自收":
                    sock.sendto(rf_rx_control_packet(False), (board, CTRL_PORT))
                sock.sendto(monitor_source_packet(1), (board, CTRL_PORT))
        except OSError as exc:
            self.events.put(("monitor_error", str(exc)))
        except ValueError as exc:
            self.events.put(("monitor_error", str(exc)))
        finally:
            # Always unwind a physical RX profile even if the UDP receive loop
            # fails after RFRX was enabled (for example a local socket error).
            try:
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as cleanup:
                    cleanup.sendto(b"STR0", (board, CTRL_PORT))
                    cleanup.sendto(loopback_control_packet(False), (board, CTRL_PORT))
                    if monitor_source == "RX ADC·外接SMA自收":
                        cleanup.sendto(rf_rx_control_packet(False), (board, CTRL_PORT))
                    cleanup.sendto(monitor_source_packet(1), (board, CTRL_PORT))
            except (OSError, ValueError):
                pass
            self.monitor_frame_pending.clear()
            self.events.put(("monitor_stopped", None))

    def _render_monitor_frame(self, value: object) -> None:
        if np is None or self.monitor_canvas is None or not isinstance(value, dict):
            return
        payload = value.get("payload")
        if not isinstance(payload, bytes):
            return
        samples = np.frombuffer(payload, dtype="<i2")
        if samples.size == 0 or samples.size % 4:
            return
        groups = samples.reshape((-1, 4))
        channel = 0
        offset = 0
        i_data = groups[:, offset].astype(np.float64)
        q_data = groups[:, offset + 1].astype(np.float64)
        if len(groups) < 4:
            return
        observation = analyze_monitor_iq(i_data, q_data, IQ_SAMPLE_RATE_HZ)
        normalized_i = observation.iq.real
        normalized_q = observation.iq.imag

        if self.monitor_artists is None:
            self._initialize_monitor_artists()
        if self.monitor_artists is None:
            return

        time_count = min(
            max(2, int(round(MONITOR_TIME_WINDOW_US * IQ_SAMPLE_RATE_HZ / 1e6)) + 1),
            len(groups))
        time_us = np.arange(time_count) * (1e6 / IQ_SAMPLE_RATE_HZ)
        axis_time, axis_spectrum = self.monitor_axes
        self.monitor_artists["time_i"].set_data(time_us, normalized_i[:time_count])
        self.monitor_artists["time_q"].set_data(time_us, normalized_q[:time_count])
        source_label = "RX ADC" if self.monitor_active_source.startswith("RX ADC") else "TX DAC"
        rx_channel, tx_channel = self._rf_ports()
        physical_channel = rx_channel if source_label == "RX ADC" else tx_channel
        axis_time.set_title(f"{source_label} CH{physical_channel} I/Q时域（0–10 µs）")
        axis_time.set_xlim(0.0, MONITOR_TIME_WINDOW_US)

        self.monitor_artists["spectrum"].set_data(
            observation.frequency_hz / 1e6, observation.spectrum_dbfs)
        if self.monitor_active_source.startswith("RX ADC") and self.monitor_rx_lo_hz and self.monitor_rx_bw_hz:
            axis_spectrum.set_title(
                f"RX{rx_channel}复基带频谱 · 中心 {self.monitor_rx_lo_hz / 1e6:.3f} MHz · "
                f"RX滤波带宽 {self.monitor_rx_bw_hz / 1e6:.3f} MHz")
            axis_spectrum.set_xlabel(
                f"相对RX中心 {self.monitor_rx_lo_hz / 1e6:.3f} MHz 的频偏 (MHz)")
        else:
            axis_spectrum.set_title(f"复基带频谱 · {observation.bin_hz / 1e3:.3g} kHz/bin")
            axis_spectrum.set_xlabel("相对本振的频偏 (MHz)")
        self.monitor_artists["spectrum_peaks"].set_offsets(np.array([
            (peak.frequency_hz / 1e6, peak.level_dbfs)
            for peak in observation.peaks]).reshape((-1, 2)))
        self.monitor_artists["spectrum_note"].set_text("\n".join(
            f"{peak.frequency_hz / 1e6:+.3f} MHz  {peak.level_dbfs:.1f} dBFS"
            for peak in observation.peaks))
        # Use the last submitted transaction, not un-applied edits. Mode is
        # still only a UI hint: the capture protocol has no mode readback.
        signature = self.__dict__.get("applied_waveform_signature")
        mode_hint = signature[channel] if signature and len(signature) > channel else None
        note = self.__dict__.get("monitor_plot_note")
        if note is not None:
            summary = spectral_peak_summary(observation, cw=bool(mode_hint and "CW" in mode_hint))
            if self.monitor_active_source.startswith("RX ADC") and self.monitor_rx_lo_hz:
                occupied = estimate_occupied_bandwidth(observation)
                if occupied is not None:
                    offset_hz, width_hz = occupied
                    summary = (f"候选信号谱中心 {(self.monitor_rx_lo_hz + offset_hz) / 1e6:.3f} MHz，"
                               f"99%占用带宽 {width_hz / 1e6:.3f} MHz（FFT估计，非校准/解调证明）；" + summary)
                else:
                    summary = "未检出足够显著的带限谱结构，不给出信号中心/B99；" + summary
            note.set(summary)
        # Synchronous draw is intentional: monitor_frame_pending is cleared
        # only after the canvas really consumed this frame, so the worker
        # cannot outrun Tk and accumulate delayed draw requests.
        self.monitor_canvas.draw()

        render_time = time.monotonic()
        self.monitor_render_count += 1
        if self.monitor_first_render_time is None:
            self.monitor_first_render_time = render_time
        render_elapsed = max(render_time - self.monitor_first_render_time, 1e-6)
        render_fps = (1.0 if self.monitor_render_count == 1 else
                      (self.monitor_render_count - 1) / render_elapsed)

        rms = math.sqrt(float(np.mean(normalized_i ** 2 + normalized_q ** 2)))
        ac_rms = math.sqrt(float(np.var(normalized_i) + np.var(normalized_q)))
        peak = float(np.max(np.abs(observation.iq)))
        incomplete = int(value.get("incomplete", 0) or 0)
        invalid = int(value.get("invalid", 0) or 0)
        if incomplete or invalid:
            self.loopback_status.set(
                f"采集链路降级：不完整帧={incomplete}、无效包={invalid}。"
                "请先降低刷新率，再检查UDP接收缓冲和串口[dma_rx]错误。")
        elif ac_rms < (2.0 / 2048.0):
            if self.monitor_active_source.startswith("RX ADC"):
                self.loopback_status.set(
                    "UDP与DMA正常，但RX I/Q接近静止/直流。请核对RFRX/LO读回、"
                    "对应SMA通道、衰减器及串口[tx-check] MON中的rx_seen=1。")
            else:
                self.loopback_status.set(
                    f"UDP与DMA正常：{self.monitor_active_source}持续到达，但I/Q为静止/直流。"
                    "请确认串口出现[txb1] START且ILA的dac_valid、DAC I/Q正在变化。")
        else:
            self.loopback_status.set(
                ("UDP与DMA正常，RX I/Q存在交流变化（也可能是噪声）；"
                 "尚不能据此确认TX信号已接收，请核对LO、谱峰和开/关发射对比。")
                if self.monitor_active_source.startswith("RX ADC") else
                f"UDP与DMA正常，{self.monitor_active_source}存在交流变化；这是LVDS之前的数字样本，不证明RF发射。")
        self.loopback_stats.set(
            f"capture={value.get('capture_id')}，{len(groups)}组RX{rx_channel}/TX{tx_channel}样本（逻辑槽0），"
            f"完整帧={value.get('completed')}/{value.get('requested')}，"
            f"不完整={value.get('incomplete')}，无效包={value.get('invalid')}；"
            f"采集={float(value.get('actual_fps', 0.0)):.2f}、绘图={render_fps:.2f} frame/s，"
            f"UDP≈{float(value.get('network_mbps', 0.0)):.2f} Mbit/s，"
            f"RMS={rms:.3f} FS，AC RMS={ac_rms:.3f} FS，峰值={peak:.3f} FS。")

    def _build_log(self, parent: ttk.Frame) -> None:
        frame = ttk.LabelFrame(parent, text="运行记录", padding=7)
        frame.pack(fill="both", expand=False)
        self.log_widget = tk.Text(frame, height=3, wrap="word", state="disabled",
                                  font=("Consolas", 9), background="#111827", foreground="#d1fae5",
                                  insertbackground="white")
        scroll = ttk.Scrollbar(frame, orient="vertical", command=self.log_widget.yview)
        self.log_widget.configure(yscrollcommand=scroll.set)
        self.log_widget.pack(side="left", fill="both", expand=True)
        scroll.pack(side="right", fill="y")
        self._log("界面已启动；默认双TX通过AD9361射频通道门控关闭。")

    @staticmethod
    def _label_entry(parent: ttk.Frame, row: int, label: str, variable: tk.StringVar) -> None:
        base = row * 2
        ttk.Label(parent, text=label).grid(row=base, column=0, sticky="w", pady=(8 if row else 0, 0))
        ttk.Entry(parent, textvariable=variable, width=18).grid(
            row=base, column=1, sticky="w", padx=(8, 22), pady=(8 if row else 0, 0))

    def _selected_standard_mode(self) -> tuple[str | None, bool]:
        selected = [mode for mode in (self.tx1_mode.get(), self.tx2_mode.get())
                    if mode in STANDARD_BIT_MODES]
        unique = set(selected)
        return (selected[0] if len(unique) == 1 else None, len(unique) > 1)

    def _current_waveform_signature(self) -> tuple[str, ...]:
        """Fingerprint the UI fields that determine the active TX waveform."""
        fields = tuple(str(variable.get()) for variable in (
            self.tx1_mode, self.tx2_mode, self.tx_lo_mhz, self.tx_bw_mhz,
            self.tx1_power_dbm, self.tx2_power_dbm,
            self.tx1_zero_attenuation_dbm, self.tx2_zero_attenuation_dbm,
            self.reference_source,
            self.source_type, self.valid_bits, self.hex_data, self.bin_path,
            self.frame_sync_enabled, self.frame_sync_length,
            self.frame_sync_word, self.minor_frame_bytes, self.id_frame_count,
            self.sfid_start, self.profile,
            self.bit_rate_mbps, self.repeat_mode, self.repeat_count, self.bit_channel,
            self.pcm_deviation_mhz, self.pcm_frequency_offset_mhz,
            self.pcm_amplitude, self.line_code, self.soqpsk_differential,
            self.pn_seed, self.pn_invert))
        if self.source_type.get() != "BIN文件":
            return fields
        try:
            digest = hashlib.sha256(Path(self.bin_path.get()).read_bytes()).hexdigest()
        except OSError:
            digest = "BIN不可读取"
        return (*fields, digest)

    def _sync_bit_channel(self, mode: str | None) -> None:
        if mode is not None and self.tx1_mode.get() == mode:
            self.bit_channel.set("TX1")

    def _refresh_line_code_control(self, mode: str | None) -> None:
        state, guidance = line_code_ui_state(mode)
        self.line_code_scope.set(guidance)
        for name in ("line_code_box", "quick_line_code_box"):
            widget = getattr(self, name, None)
            if widget is not None:
                widget.configure(state=state)

    def _update_power_summaries(self) -> None:
        _, tx_channel = self._rf_ports()
        channels = (
            (f"TX{tx_channel}", self.tx1_mode, self.tx1_power_dbm,
             self.tx1_zero_attenuation_dbm, self.tx1_power_summary),
            ("禁用逻辑槽1", self.tx2_mode, self.tx2_power_dbm,
             self.tx2_zero_attenuation_dbm, self.tx2_power_summary),
        )
        for name, mode, power, calibration, summary in channels:
            if mode.get() == "不发射":
                summary.set(f"{name}射频通道门控关闭；功率值仅保留，不用于静音。")
                continue
            try:
                requested = float(power.get())
                full_scale = float(calibration.get())
                attenuation = power_to_attenuation_db(requested, full_scale)
                validate_cw_power(mode.get(), attenuation, full_scale)
                applied = attenuation_to_power_dbm(attenuation, full_scale)
            except ValueError as exc:
                summary.set(str(exc))
                continue
            summary.set(
                f"按校准值估算{applied:g} dBm（AD9361衰减{attenuation:g} dB）；非整机实测功率。")

    def _apply_preset(self, mode: str, *, log_change: bool = True) -> None:
        preset = recommended_parameters(mode, self.profile.get())
        if preset is None:
            return
        self.profile.set(str(preset["profile"]))
        self.bit_rate.set(str(preset["bit_rate"]))
        self.pcm_deviation_hz.set(str(preset["deviation_hz"]))
        self.pcm_frequency_offset_hz.set(str(preset["offset_hz"]))
        self.pcm_amplitude.set(str(preset["amplitude"]))
        self.soqpsk_differential.set(bool(preset["differential"]))
        self.line_code.set(str(preset.get("line_code", "NRZ-L")))
        self.rolloff.set(str(preset.get("rolloff", "标准/无（PCM/FM Bessel）")))
        self._refresh_line_code_control(mode)
        self._sync_bit_channel(mode)
        self.preset_summary.set(f"智能匹配完成：{preset['description']}。请查看参数后手动确认下发。")
        if log_change:
            self._log(f"已按{mode}自动匹配参数：{preset['description']}；尚未向板卡下发。")

    def _mode_changed(self) -> None:
        self._update_power_summaries()
        mode, conflict = self._selected_standard_mode()
        self._update_rf_bandwidth_summary()
        if conflict:
            self._refresh_line_code_control(None)
            self.preset_summary.set("模式冲突：PCM/FM和SOQPSK-TG共享一条BIT链，双路不能混选。")
            self._log("检测到BIT模式冲突；1R1T仅允许第一个逻辑槽，第二槽必须关闭。")
            return
        if mode is None:
            self._refresh_line_code_control(None)
            active_legacy = [item for item in (self.tx1_mode.get(), self.tx2_mode.get())
                             if item not in ("不发射", *STANDARD_BIT_MODES)]
            if active_legacy:
                self.preset_summary.set("当前为旧DDS诊断模式，不使用指定BIT滤波/Profile参数。")
            else:
                self.preset_summary.set("智能匹配：选择标准BIT调制模式后自动填入推荐参数；确认前不会下发。")
            return
        self._refresh_line_code_control(mode)
        self._sync_bit_channel(mode)
        if self.auto_preset.get():
            self._apply_preset(mode)
        else:
            self.preset_summary.set(f"已选择{mode}，自动匹配关闭；请人工核对Profile和调制参数。")

    def _bit_rate_changed(self) -> None:
        """Keep the mandatory PCM/FM deviation coupled to Rb in standard mode."""
        if self._board_sync_busy:
            return
        self._update_summary()
        self._update_rf_bandwidth_summary()
        mode, conflict = self._selected_standard_mode()
        if conflict or mode != "PCM/FM（PL）" or not self.auto_preset.get():
            return
        try:
            bit_rate = int(self.bit_rate.get())
        except ValueError:
            return
        if PCM_MIN_BIT_RATE <= bit_rate <= PCM_MAX_BIT_RATE:
            self.pcm_deviation_hz.set(str(int(round(0.35 * bit_rate))))
            self.preset_summary.set(
                f"PCM/FM连续码率：Rb={bit_rate / 1e6:g} Mbit/s，"
                f"已自动匹配Δf=0.35Rb={0.35 * bit_rate / 1e6:g} MHz。请确认后下发。")

    def _update_rf_bandwidth_summary(self) -> None:
        try:
            rf_bw_mhz = float(self.tx_bw_mhz.get())
        except ValueError:
            self.rf_bw_summary.set("请输入0.2–56 MHz的AD9361 TX滤波带宽。")
            return
        if not AD9361_TX_BW_MIN_MHZ <= rf_bw_mhz <= AD9361_TX_BW_MAX_MHZ:
            self.rf_bw_summary.set("AD9361 TX滤波带宽必须在0.2–56 MHz。")
            return
        self.rf_bw_summary.set(
            f"AD9361 TX模拟滤波带宽{rf_bw_mhz:g} MHz（允许0.2–56 MHz）；"
            "它不等于调制占用带宽，实际占用带宽需由频谱仪测量。")

    def _endpoint(self) -> tuple[str, str]:
        board = FIXED_BOARD_IP
        if not self.local_ip.get().strip():
            raise ValueError("未获取本机IPv4地址，请连接网线并配置网卡后点击刷新本机IP，或手动填写本机网卡地址。")
        local = str(ipaddress.IPv4Address(self.local_ip.get().strip()))
        if local == "0.0.0.0" or board == local:
            raise ValueError("请填写独立的板卡IP和本机以太网IPv4地址，不能绑定0.0.0.0。")
        return board, local

    @staticmethod
    def _phase_word(freq_hz: float) -> int:
        return int(round(freq_hz * (1 << 32) / IQ_SAMPLE_RATE_HZ)) & 0xFFFFFFFF

    def _pcm_packet(self, enabled: bool) -> bytes:
        if not enabled:
            # Disabling PCM must work even when unrelated editable fields are
            # temporarily invalid. Disabled parameters are ignored by PL/PS.
            return struct.pack("<4sBBBBIIII", b"PCMF", 2, 0, 0, 0,
                               0, 0, 0, 0)
        try:
            deviation_hz = float(self.pcm_deviation_hz.get())
            offset_hz = float(self.pcm_frequency_offset_hz.get())
            amplitude = float(self.pcm_amplitude.get())
            bit_rate = int(self.bit_rate.get())
        except ValueError as exc:
            raise ValueError("PCM/FM参数必须是数字") from exc
        if not 0 <= deviation_hz <= IQ_SAMPLE_RATE_HZ / 2:
            raise ValueError("PCM/FM频偏范围为0–30.72 MHz")
        if not -IQ_SAMPLE_RATE_HZ / 2 <= offset_hz < IQ_SAMPLE_RATE_HZ / 2:
            raise ValueError("PCM/FM基带偏移范围为-30.72–30.72 MHz")
        if not 0 <= amplitude <= 1:
            raise ValueError("PCM/FM幅度范围为0–1")
        amplitude_word = int(round(amplitude * 0x7FFF))
        if enabled and amplitude_word == 0:
            raise ValueError("PCM/FM发射数字幅度不能为0或量化为0；停发请使用停止发射按钮")
        profile = PROFILE_VALUE[self.profile.get()]
        if enabled and profile != 2:
            raise ValueError("PCM/FM模式需要选择标准4阶Bessel连续码率Profile")
        if enabled:
            if not PCM_MIN_BIT_RATE <= bit_rate <= PCM_MAX_BIT_RATE:
                raise ValueError("PCM/FM BIT率范围为10 kbit/s–20 Mbit/s")
            if self.line_code.get() not in LINE_CODE_VALUE:
                raise ValueError("PCM/FM码型必须为NRZ-L/M/S、RNRZ-L或Biφ-L/M/S")
            expected_deviation = 0.35 * bit_rate
            if abs(deviation_hz - expected_deviation) > 1:
                raise ValueError(f"标准PCM/FM要求频偏Δf=0.35Rb={expected_deviation:g} Hz")
            if abs(offset_hz) > 0.5:
                raise ValueError("标准模式请用TX LO设置载频，PCM/FM基带偏移必须为0 Hz")
        filter_profile = 0
        randomizer = int(enabled and self.line_code.get() == "RNRZ-L（IRIG随机化）")
        rate_word = self._phase_word(bit_rate)
        # Match PS integer arithmetic; display rounding must not alter h=0.7.
        deviation_word = (rate_word * 35 + 50) // 100
        return struct.pack("<4sBBBBIIII", b"PCMF", 2, int(enabled),
                           filter_profile, randomizer, rate_word,
                           deviation_word,
                           self._phase_word(offset_hz),
                           amplitude_word)

    def _soqpsk_packet(self, enabled: bool) -> bytes:
        try:
            bit_rate = int(self.bit_rate.get())
            offset_hz = float(self.pcm_frequency_offset_hz.get())
            amplitude = float(self.pcm_amplitude.get())
        except ValueError as exc:
            raise ValueError("SOQPSK的BIT率、偏移和幅度必须是数字") from exc
        if enabled and not SOQPSK_MIN_BIT_RATE <= bit_rate <= SOQPSK_MAX_BIT_RATE:
            raise ValueError("SOQPSK-TG BIT率范围为10 kbit/s–20 Mbit/s")
        if not -IQ_SAMPLE_RATE_HZ / 2 <= offset_hz < IQ_SAMPLE_RATE_HZ / 2:
            raise ValueError("SOQPSK基带偏移范围为-30.72–30.72 MHz")
        if not 0 <= amplitude <= 1:
            raise ValueError("SOQPSK幅度范围为0–1")
        amplitude_word = int(round(amplitude * 0x7FFF))
        if enabled and amplitude_word == 0:
            raise ValueError("SOQPSK发射数字幅度不能为0或量化为0；停发请使用停止发射按钮")
        if enabled and abs(offset_hz) > 0.5:
            raise ValueError("标准模式请用TX LO设置载频，SOQPSK基带偏移必须为0 Hz")
        if enabled and not self.soqpsk_differential.get():
            raise ValueError("IRIG标准SOQPSK-TG必须启用差分编码")
        rate_word = self._phase_word(bit_rate) if enabled else 0
        return struct.pack("<4sBBBBIII", b"SQPK", 1, int(enabled),
                           int(self.soqpsk_differential.get()), int(enabled), rate_word,
                           self._phase_word(offset_hz), amplitude_word)

    def _bit_modulation_selection(self) -> tuple[bool, bool]:
        modes = {MODE_VALUE[self.tx1_mode.get()], MODE_VALUE[self.tx2_mode.get()]}
        pcm_selected = 4 in modes
        soqpsk_selected = 3 in modes
        if pcm_selected and soqpsk_selected:
            raise ValueError("1R1T只有一条指定BIT调制链，第二逻辑槽必须关闭")
        return pcm_selected, soqpsk_selected

    def _new_socket(self) -> socket.socket:
        _board, local = self._endpoint()
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            sock.bind((local, 0))
        except OSError:
            sock.close()
            raise
        return sock

    def _log(self, text: str) -> None:
        stamp = datetime.now().strftime("%H:%M:%S")
        self.log_widget.configure(state="normal")
        self.log_widget.insert("end", f"[{stamp}] {text}\n")
        self.log_widget.see("end")
        self.log_widget.configure(state="disabled")

    def _error(self, title: str, exc: object) -> None:
        self._set_operation_status("error", f"失败：{exc}")
        self._log(f"错误：{exc}")
        messagebox.showerror(title, str(exc), parent=self)

    def _notify_apply_complete(self) -> None:
        """Report command-sequence completion without claiming a board ACK."""
        if self.__dict__.get("network_busy", False):
            return
        active = [mode for mode in (self.tx1_mode.get(), self.tx2_mode.get())
                  if mode != "不发射"]
        mode_text = " + ".join(active) if active else "双TX关闭"
        self._set_operation_status("sent", f"指令已下发、射频待核对：{mode_text}")
        messagebox.showinfo(
            "下发完成",
            f"{mode_text} 的配置与启动命令序列已完成发送。\n"
            "连接测试有往返应答，但不代表每条发射命令执行成功；"
            "需要硬件回读时请使用“查询BIT/RF状态”。",
            parent=self)

    def send_control(self, payload: bytes, success: str) -> bool:
        self._control_generation += 1
        try:
            board, _local = self._endpoint()
            with self._new_socket() as sock:
                sock.sendto(payload, (board, CTRL_PORT))
        except (OSError, ValueError) as exc:
            self._error("发送失败", exc)
            return False
        self._log(f"{success}；已发送{len(payload)}字节到{board}:{CTRL_PORT}")
        return True

    def test_connection(self) -> None:
        self._start_network_operation(reset=False)

    def refresh_board_configuration(self) -> None:
        """后台只读查询；不调用STOP、REFC、TXCF、START或PING。"""
        if self.config_busy or self.worker_active or self.network_busy or self.monitor_running:
            self.config_status.set("当前配置：操作进行中，请空闲时读取")
            return
        try:
            board, local = self._endpoint()
        except ValueError:
            self.config_status.set("当前配置：未连接；界面值未获板卡确认")
            return
        self.config_busy = True
        token = (self._current_waveform_signature(), self._control_generation, board, local)
        self.config_status.set("当前配置：正在只读查询…")
        def worker() -> None:
            try:
                result = (read_config(board, local), "", token)
            except (OSError, ValueError, RuntimeError) as exc:
                result = (None, str(exc), token)
            self.events.put(("board_config", result))
        threading.Thread(target=worker, daemon=True).start()

    def _rf_ports(self) -> tuple[int, int]:
        return (self.__dict__.get("physical_rx_channel", 2),
                self.__dict__.get("physical_tx_channel", 2))

    def _set_rf_ports(self, rx: int, tx: int) -> None:
        self.physical_rx_channel, self.physical_tx_channel = rx, tx
        self.rf_ports_verified = True
        self.rf_channel_title.set(f"TX{tx}（1R1T）")
        self.tx_channel_frame.configure(text=self.rf_channel_title.get())
        self.monitor_channel_title.set(f"TX{tx} / RX{rx}（1R1T）")
        self.monitor_route_hint.set(
            f"固定 5 frame/s · 61.44 MSPS · 时域0–10 µs。外接SMA：TX{tx}A→RX{rx}A，需固定衰减器。")

    def _apply_board_configuration(self, config, token) -> bool:
        # 编辑/发射期间到达的旧响应不能覆盖用户输入，也不能把已停发任务显示成运行。
        signature, generation, board, local = token
        if (self.worker_active or self.network_busy or self.monitor_running or
            generation != self._control_generation or signature != self._current_waveform_signature() or
            (board, local) != (self.board_ip.get(), self.local_ip.get())):
            self.config_status.set("当前配置：查询期间已有编辑/操作，未覆盖；可重新读取")
            return False
        self._board_sync_busy = True
        try:
            self._set_rf_ports(*config.rf_channels)
            for name, value in config.ui_fields().items():
                getattr(self, name).set(value)
        finally:
            self._board_sync_busy = False
        self._source_changed()
        self._frame_sync_changed()
        self._repeat_changed()
        mode, _ = self._selected_standard_mode()
        self._refresh_line_code_control(mode)
        self._update_power_summaries()
        self._update_rf_bandwidth_summary()
        self._update_summary()
        flags = config.get('flags')
        self.reference_status.set("板端参考配置已读回" if flags & 1 else "板端参考无效，禁止发射")
        source = ('本地PN11与组帧已读取' if config.get('source') == 1 else
                  '下载数据内容/组帧未知，界面数据源仅为本机编辑值' if config.get('source') == 2 else
                  '无已提交任务，界面数据源仅为本机编辑值')
        state = '任务运行' if flags & 32 else '任务未运行'
        if flags & 64:
            state = '任务报错'
        rx, tx = config.rf_channels
        self.config_status.set(f"已读取板端配置 · TX{tx}A/RX{rx}A · {state} · 功率为校准估算，非实测")
        self.preset_summary.set(f"板端只读回读：{source}。模式与任务状态分别显示；参数未重新下发。")
        self._log(f"只读配置：TX{tx}A/RX{rx}A，LO={config.lo_hz / 1e6:g} MHz；{source}；{state}。"
                  "未下发任何模式、时钟或启动命令。")
        self.applied_waveform_signature = (self._current_waveform_signature()
            if flags & 1 and flags & 32 and not flags & (64 | 128) and config.get('source') == 1 else None)
        return True

    def reset_connection(self) -> None:
        self._start_network_operation(reset=True)

    def _start_network_operation(self, *, reset: bool) -> None:
        if self.network_busy:
            return
        try:
            board, local = self._endpoint()
        except ValueError as exc:
            self._error("网络配置错误", exc)
            return
        self.network_busy = True
        self.network_test_button.configure(state="disabled")
        self.network_reset_button.configure(state="disabled")
        self._set_connection_status("busy", "正在重置连接…" if reset else "正在等待板卡应答…")
        if reset:
            # Do not resume stale snapshots or a partially downloaded task.
            self.transfer_cancel.set()
            self.monitor_stop.set()
            self.monitor_pause.clear()
            self.monitor_start_pending = False
            self.applied_waveform_signature = None
            self._log("重置连接：停止上位机发送/监视线程，重建板端GEM通信队列；"
                      "不修改RF、功率和正在运行的PL调制。完成后手动重新应用或开启监视。")
        threading.Thread(target=self._network_worker,
                         args=(board, local, reset), daemon=True).start()

    def _network_worker(self, board: str, local: str, reset: bool) -> None:
        try:
            if reset:
                for worker in (self.transfer_thread, self.monitor_thread):
                    if worker is not None and worker.is_alive():
                        worker.join(timeout=3.0)
                        if worker.is_alive():
                            raise RuntimeError("旧发送线程仍未退出，未执行板端重置；请关闭并重开UI。")
                udp_round_trip(board, local, b"NRES", b"NRAK")
            elapsed = udp_round_trip(board, local)
        except (OSError, ValueError, RuntimeError) as exc:
            self.events.put(("network_result", {"ok": False, "reset": reset,
                                               "error": str(exc)}))
        else:
            self.events.put(("network_result", {"ok": True, "reset": reset,
                                               "elapsed": elapsed}))

    def _read_rf_values(self) -> tuple[float, float, float, float]:
        lo = float(self.tx_lo_mhz.get())
        bw = float(self.tx_bw_mhz.get())
        if not 70 <= lo <= 6000:
            raise ValueError("TX LO范围为70–6000 MHz")
        if not AD9361_TX_BW_MIN_MHZ <= bw <= AD9361_TX_BW_MAX_MHZ:
            raise ValueError("AD9361 TX模拟滤波带宽范围为0.2–56 MHz")
        # OFF is a real AD9361 channel/ENSM gate in PS, so an inactive channel
        # never depends on its power text being valid.  Active channels convert
        # calibrated connector power back to the relative register attenuation.
        att1 = (0.0 if self.tx1_mode.get() == "不发射" else
                power_to_attenuation_db(
                    float(self.tx1_power_dbm.get()),
                    float(self.tx1_zero_attenuation_dbm.get())))
        att2 = (0.0 if self.tx2_mode.get() == "不发射" else
                power_to_attenuation_db(
                    float(self.tx2_power_dbm.get()),
                    float(self.tx2_zero_attenuation_dbm.get())))
        if self.tx1_mode.get() != "不发射":
            validate_cw_power(self.tx1_mode.get(), att1, float(self.tx1_zero_attenuation_dbm.get()))
        if self.tx2_mode.get() != "不发射":
            validate_cw_power(self.tx2_mode.get(), att2, float(self.tx2_zero_attenuation_dbm.get()))
        return lo, bw, att1, att2

    def _power_log_value(self, channel: int, attenuation_db: float) -> str:
        mode = self.tx1_mode.get() if channel == 1 else self.tx2_mode.get()
        if mode == "不发射":
            return "RF关闭"
        calibration = (self.tx1_zero_attenuation_dbm.get() if channel == 1 else
                       self.tx2_zero_attenuation_dbm.get())
        return f"{attenuation_to_power_dbm(attenuation_db, float(calibration)):g}dBm"

    def _txcf_packet(self) -> bytes:
        """Build the complete RF/mode command for RF Apply and BIT start."""
        lo, bw, att1, att2 = self._read_rf_values()
        try:
            line_code = LINE_CODE_VALUE[self.line_code.get()]
            rolloff = ROLLOFF_VALUE[self.rolloff.get()]
        except KeyError as exc:
            raise ValueError("请选择有效的输入编码和滚降预设") from exc
        shape = line_code | (rolloff << 4)
        return struct.pack("<4sBBHQIII", b"TXCF", MODE_VALUE[self.tx1_mode.get()],
                           MODE_VALUE[self.tx2_mode.get()], shape, round(lo * 1_000_000),
                           round(bw * 1_000_000), round(att1 * 1000), round(att2 * 1000))

    @staticmethod
    def _emergency_off_packet() -> bytes:
        """仅关门控：不改变LO/带宽，不在紧急停发时触发内部校准音。"""
        return b"TXM\x00\x00"

    def _abort_rf_worker(self, board: str, local: str) -> None:
        """超时/失败后尽力停发；UDP送出不等于硬件已经关闭。"""
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                sock.bind((local, 0))
                sock.sendto(self._emergency_off_packet(), (board, CTRL_PORT))
        except OSError:
            self.events.put(("log", "停发命令未成功送出，请检查板端RF状态。"))

    def _cancel_rf_worker(self, board: str, local: str) -> None:
        self._abort_rf_worker(board, local)
        self.events.put(("transfer_cancelled", None))

    def _refclk_packet(self) -> bytes:
        """Build the reference-clock command (internal 40 MHz or external 10 MHz)."""
        try:
            source, rate_hz = REFERENCE_VALUE[self.reference_source.get()]
        except KeyError as exc:
            raise ValueError("请选择有效的参考时钟源") from exc
        return struct.pack("<4sBBHI", b"REFC", 1, source, 0, rate_hz)

    def apply_tx(self) -> None:
        if self.__dict__.get("network_busy", False):
            self._log("正在检测/重置连接，请完成后再应用发射。")
            return
        try:
            pcm_enabled, soqpsk_enabled = self._bit_modulation_selection()
            standard_enabled = pcm_enabled or soqpsk_enabled
            self._commit_mega_fields(
                require_rate=standard_enabled,
                require_deviation=pcm_enabled,
                require_offset=standard_enabled)
            lo, bw, att1, att2 = self._read_rf_values()
        except ValueError as exc:
            self._error("参数错误", exc)
            return
        if self.worker_active:
            messagebox.showinfo("任务进行中", "BIT事务尚未结束。请等待完成，或先按停止/紧急关闭。", parent=self)
            return
        self._set_operation_status("busy", "正在应用配置…")
        if standard_enabled:
            self._log("检测到标准BIT调制：先下载并COMMIT当前BIT，再切换波形，最后START。")
            self.start_bit_transfer()
            return
        self._start_reference_worker(clock_only=False)

    def apply_reference(self) -> None:
        """Apply/recover the reference independently of waveform parameters."""
        self._start_reference_worker(clock_only=True)

    def _start_reference_worker(self, *, clock_only: bool) -> None:
        if self.worker_active or self.network_busy:
            self._log("正在处理控制事务，请完成后再切换参考时钟。")
            return
        try:
            board, local = self._endpoint()
            refclk = self._refclk_packet()
            txcf = None if clock_only else self._txcf_packet()
            pcm_off = None if clock_only else self._pcm_packet(False)
            signature = None if clock_only else self._current_waveform_signature()
        except ValueError as exc:
            self._error("参数错误", exc)
            return
        self.worker_active = True
        self._control_generation += 1
        self.transfer_cancel.clear()
        self.applied_waveform_signature = None
        self.send_button.configure(state="disabled")
        self.reference_status.set("正在确认参考时钟，等待板端校验…")
        self._set_operation_status("busy", "正在配置参考时钟…")

        def worker() -> None:
            try:
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                    sock.bind((local, 0))
                    if self.monitor_running and not self.monitor_stop.is_set():
                        self.monitor_quiesced.clear()
                        self.monitor_pause.set()
                        sock.sendto(b"STR0", (board, CTRL_PORT))
                        self.monitor_quiesced.wait(timeout=1.0)
                        time.sleep(MONITOR_QUIESCE_SETTLE_S)
                    detail = confirm_reference(sock, (board, CTRL_PORT), refclk)
                    self.events.put(("reference_status", detail))
                    if self.transfer_cancel.is_set():
                        self._cancel_rf_worker(board, local)
                        return
                    if txcf is not None:
                        sock.sendto(txcf, (board, CTRL_PORT))
                        time.sleep(MODE_CONFIG_SETTLE_S)
                        if self.transfer_cancel.is_set():
                            self._cancel_rf_worker(board, local)
                            return
                        sock.sendto(pcm_off, (board, CTRL_PORT))
                        confirm_rf_config(board, local, txcf, port=CTRL_PORT,
                                          cancelled=self.transfer_cancel.is_set)
                    self.events.put(("rf_done", signature))
            except (OSError, ValueError) as exc:
                if txcf is not None:
                    self._abort_rf_worker(board, local)
                self.events.put(("transfer_cancelled", None) if self.transfer_cancel.is_set()
                                else ("rf_error", str(exc)))
        self.transfer_thread = threading.Thread(target=worker, daemon=True)
        self.transfer_thread.start()

    def emergency_off(self) -> None:
        self.monitor_start_pending = False
        self.applied_waveform_signature = None
        self.tx1_mode.set("不发射")
        self.tx2_mode.set("不发射")
        self._update_power_summaries()
        self.transfer_cancel.set()
        self.send_bit_command(Opcode.STOP, quiet=True)
        self.send_bit_command(Opcode.CLEAR, quiet=True)
        gate_sent = self.send_control(self._emergency_off_packet(),
                                      "紧急关闭：双TX射频通道门控OFF")
        self.send_control(self._pcm_packet(False), "紧急关闭：标准BIT调制禁用")
        if gate_sent:
            self._log("已发送紧急关闭：BIT STOP + AD9361双TX射频门控OFF；请查询板端状态。")
            self._set_operation_status("stopped", "双TX已发送关闭命令，等待板端核对")
        else:
            self._set_operation_status("error", "关闭命令未成功发送，请立即检查板卡")

    def _source_changed(self) -> None:
        kind = self.source_type.get()
        self.hex_entry.configure(state="normal" if kind == "HEX" else "disabled")
        state = "normal" if kind == "BIN文件" else "disabled"
        self.bin_entry.configure(state=state)
        self.bin_button.configure(state=state)
        pn_state = "normal" if kind in PN_POLYNOMIALS else "disabled"
        self.pn_seed_entry.configure(state=pn_state)
        self.pn_invert_button.configure(state=pn_state)
        self._update_summary()

    def _frame_sync_changed(self) -> None:
        enabled = self.frame_sync_enabled.get()
        state = "normal" if enabled else "disabled"
        self.frame_sync_length_entry.configure(state=state)
        self.frame_sync_word_entry.configure(state=state)
        self.minor_frame_bytes_entry.configure(state=state)
        self.id_frame_count_entry.configure(state=state)
        self.sfid_start_entry.configure(state=state)
        self.valid_bits_entry.configure(state="disabled" if enabled else "normal")
        self.quick_valid_bits_entry.configure(state="disabled" if enabled else "normal")
        self._update_summary()

    def _repeat_changed(self) -> None:
        self.repeat_entry.configure(state="normal" if self.repeat_mode.get() == "指定附加次数" else "disabled")

    def choose_bin(self) -> None:
        path = filedialog.askopenfilename(title="选择BIT二进制文件",
                                          filetypes=(("Binary", "*.bin"), ("All files", "*.*")))
        if path:
            self.bin_path.set(path)
            size = Path(path).stat().st_size
            if not self.frame_sync_enabled.get():
                self.valid_bits.set(str(size * 8))
            self._update_summary()

    def choose_ila_csv(self) -> None:
        path = filedialog.askopenfilename(
            title="选择Vivado ILA导出的CSV",
            filetypes=(("ILA CSV", "*.csv"), ("All files", "*.*")))
        if path:
            self.ila_csv_path.set(path)

    def choose_ila_reference(self) -> None:
        path = filedialog.askopenfilename(
            title="选择BER参考BIT二进制文件",
            filetypes=(("Binary", "*.bin"), ("All files", "*.*")))
        if path:
            reference = Path(path)
            self.ila_reference_path.set(path)
            self.ila_reference_bits.set(str(reference.stat().st_size * 8))

    def use_current_ila_reference(self) -> None:
        self.ila_reference_path.set("")
        self.ila_reference_bits.set("")
        self._log("ILA分析将使用指定BIT发送页当前数据源作为BER参考。")

    def _set_ila_result(self, text: str) -> None:
        self.ila_result_widget.configure(state="normal")
        self.ila_result_widget.delete("1.0", "end")
        self.ila_result_widget.insert("1.0", text)
        self.ila_result_widget.configure(state="disabled")

    def _ila_reference(self) -> tuple[bytes, int]:
        reference_path = self.ila_reference_path.get().strip()
        if not reference_path:
            return self._payload()
        path = Path(reference_path)
        if not path.is_file():
            raise ValueError("请选择有效的BER参考BIN文件")
        if path.stat().st_size > 16 * 1024 * 1024:
            raise ValueError("BER参考BIN不能超过16 MiB")
        payload = path.read_bytes()
        bits_text = self.ila_reference_bits.get().strip()
        valid_bits = int(bits_text) if bits_text else len(payload) * 8
        if not 0 < valid_bits <= len(payload) * 8:
            raise ValueError("参考有效BIT数超出BIN文件范围")
        return payload, valid_bits

    def analyze_ila_capture(self) -> None:
        try:
            self._commit_mega_fields(require_deviation=False, require_offset=False)
            csv_path = Path(self.ila_csv_path.get().strip())
            if not csv_path.is_file():
                raise ValueError("请先选择有效的ILA CSV文件")
            if csv_path.stat().st_size > 128 * 1024 * 1024:
                raise ValueError("ILA CSV不能超过128 MiB")
            reference_payload, reference_bits = self._ila_reference()
            requested_rate = int(self.bit_rate.get())
            selected_line_code = self.ila_line_code.get()
            analyzed_line_code = (self.line_code.get()
                                  if selected_line_code == ILA_LINE_CODE_CURRENT
                                  else selected_line_code)
            result = analyze_ila_csv(
                csv_path,
                sample_rate_hz=IQ_SAMPLE_RATE_HZ,
                reference_payload=reference_payload,
                reference_valid_bits=reference_bits,
                requested_bit_rate=requested_rate,
                line_code=analyzed_line_code,
            )
            self._set_ila_result(format_ila_analysis(result))
            if result.ber is None:
                ber_text = "未计算"
            else:
                ber_text = f"{result.ber:.6g}"
            rate_text = (f"{result.rate_word_bit_rate / 1e6:.9g} Mbit/s"
                         if result.rate_word_bit_rate is not None else "不可用")
            self._log(
                f"ILA分析完成：有效BIT={result.accepted_events}，"
                f"长期平均码率={rate_text}，BER={ber_text}，"
                f"码型判定={('通过' if result.encoding_passed else '失败' if result.encoding_passed is False else '样本不足')}。")
        except (OSError, UnicodeError, ValueError) as exc:
            self._error("ILA CSV分析失败", exc)

    def _parse_pn_seed(self, kind: str) -> int | None:
        text = self.pn_seed.get().strip()
        if not text:
            return None
        cleaned = text[2:] if text.lower().startswith("0x") else text
        try:
            return int(cleaned, 16)
        except ValueError as exc:
            raise ValueError(f"{kind}初始值必须为十六进制数") from exc

    def _source_payload(self, requested_bits: int | None = None) -> tuple[bytes, int]:
        kind = self.source_type.get()
        if kind == "HEX":
            raw = parse_hex_text(self.hex_data.get())
            source_bits = len(raw) * 8
        elif kind == "BIN文件":
            path = Path(self.bin_path.get().strip())
            if not path.is_file():
                raise ValueError("请选择有效的BIN文件")
            if path.stat().st_size > 1024 * 1024:
                raise ValueError("BIN文件不能超过1 MiB")
            raw = path.read_bytes()
            if not raw:
                raise ValueError("BIN文件不能为空")
            source_bits = len(raw) * 8
        else:
            source_bits = requested_bits if requested_bits is not None else int(self.valid_bits.get())
            if kind in PN_POLYNOMIALS:
                raw = generate_pn_pattern(
                    kind, source_bits, seed=self._parse_pn_seed(kind),
                    invert=self.pn_invert.get())
            else:
                raw = generate_pattern(kind, source_bits)
        if requested_bits is None and kind not in ("HEX", "BIN文件"):
            return normalize_tail(raw, source_bits), source_bits
        return raw, source_bits

    def _payload(self) -> tuple[bytes, int]:
        if self.frame_sync_enabled.get():
            try:
                sync_bits = int(self.frame_sync_length.get())
                frame_bytes = int(self.minor_frame_bytes.get())
            except ValueError as exc:
                raise ValueError("同步字长度和小帧长度必须为整数") from exc
            sync_text = self.frame_sync_word.get().strip()
            if sync_text.lower().startswith("0x"):
                sync_text = sync_text[2:]
            try:
                sync_word = int(sync_text, 16)
            except ValueError as exc:
                raise ValueError("同步字必须为十六进制数") from exc
            count_var = self.__dict__.get("id_frame_count")
            frame_count = int(count_var.get()) if count_var is not None else 1
            start_var = self.__dict__.get("sfid_start")
            sfid_start = int(start_var.get()) if start_var is not None else 0
            if not 1 <= frame_count <= 256:
                raise ValueError("ID副帧数必须为1–256，1表示关闭")
            if sfid_start not in (0, 1):
                raise ValueError("SFID起始值只能为0或1")
            if frame_count > 1 and frame_bytes * frame_count > PL_REPLAY_BYTES:
                raise ValueError("ID大帧超过16 KiB循环缓存，请减少副帧数或小帧长度")
            payload_capacity = (frame_bytes * 8 - sync_bits -
                                (8 if frame_count > 1 else 0)) * frame_count
            if payload_capacity <= 0:
                raise ValueError("小帧长度必须大于同步字长度")
            if self.source_type.get() in ("HEX", "BIN文件"):
                source, source_bits = self._source_payload()
                if source_bits > payload_capacity:
                    raise ValueError(
                        f"当前HEX/BIN有{source_bits} bit，超过帧载荷{payload_capacity} bit；"
                        "请缩短数据、增大帧长度或关闭帧同步，"
                        "以免尾部BIT被静默丢弃。")
            else:
                source, source_bits = self._source_payload(payload_capacity)
            if frame_count > 1:
                return build_id_major_frame(
                    source, source_bits, sync_word=sync_word, sync_bits=sync_bits,
                    frame_bytes=frame_bytes, frame_count=frame_count,
                    sfid_start=sfid_start)
            return build_minor_frame(
                source, source_bits, sync_word=sync_word,
                sync_bits=sync_bits, frame_bytes=frame_bytes)

        bits = int(self.valid_bits.get())
        raw, source_bits = self._source_payload(bits)
        if self.source_type.get() in ("HEX", "BIN文件"):
            return normalize_tail(raw, bits), bits
        return normalize_tail(raw, source_bits), source_bits

    def _repeat_value(self) -> int:
        mode = self.repeat_mode.get()
        if mode == "单次":
            return 0
        if mode == "无限循环":
            return 0xFFFFFFFF
        value = int(self.repeat_count.get())
        if not 0 <= value < 0xFFFFFFFF:
            raise ValueError("附加重复次数必须在0到4294967294之间")
        return value

    def _task_values(self) -> tuple[int, int, float]:
        task = int(self.task_id.get())
        rate = int(self.bit_rate.get())
        requested_gap = float(self.gap_ms.get())
        if not 0 <= task <= 0xFFFFFFFF:
            raise ValueError("任务ID必须在0到4294967295之间")
        if not 1 <= rate <= MAX_BIT_RATE:
            raise ValueError("当前61.44 MHz采样时钟下，码率必须在0.000001–61.44 Mbit/s之间")
        if not 0 <= requested_gap <= 1000:
            raise ValueError("分包间隔必须在0到1000 ms之间")
        return task, rate, requested_gap

    def _update_summary(self, payload: bytes | None = None, bits: int | None = None) -> None:
        try:
            if payload is None or bits is None:
                payload, bits = self._payload()
            rate = int(self.bit_rate.get())
            if not 1 <= rate <= MAX_BIT_RATE:
                raise ValueError("BIT率超出范围")
            preview = "".join(f"{byte:08b}" for byte in payload)[:min(bits, 64)]
            suffix = "…" if bits > 64 else ""
            self.bit_summary.set(f"预期：{bits} bit / {len(payload)} byte，MSB First，前64位：{preview}{suffix}")
            samples_per_bit, duration_s = bit_timing(rate, bits)
            if duration_s < 1e-3:
                duration_text = f"{duration_s * 1e6:.3g} us"
            elif duration_s < 1:
                duration_text = f"{duration_s * 1e3:.3g} ms"
            else:
                duration_text = f"{duration_s:.3g} s"
            if samples_per_bit >= 1:
                sample_text = f"平均{samples_per_bit:.6g} I/Q样点/BIT"
            else:
                sample_text = (f"平均{samples_per_bit:.6g} I/Q样点/BIT（低于1，"
                               "不能直接生成逐BIT调制波形）")
            self.bit_timing_summary.set(
                f"码率{rate / 1e6:g} Mbit/s；61.44 MSPS下：{sample_text}；"
                f"单次时长{duration_text}；PL节拍时钟61.44 MHz")
            if self.frame_sync_enabled.get():
                sync_bits = int(self.frame_sync_length.get())
                frame_bytes = int(self.minor_frame_bytes.get())
                frame_rate = rate / (frame_bytes * 8)
                count_var = self.__dict__.get("id_frame_count")
                count = int(count_var.get()) if count_var is not None else 1
                start_var = self.__dict__.get("sfid_start")
                sfid_start = int(start_var.get()) if start_var is not None else 0
                frame_class = ("长度/码率处于Class I范围" if frame_bytes <= 1024 and rate <= 10_000_000
                               else "包含Class II特征，需接收端认可")
                pn_note = ("；PN在循环缓冲重播时从所设初值重新开始"
                           if self.source_type.get() in PN_POLYNOMIALS else "")
                id_note = (f"；{count}小帧/大帧，SFID={sfid_start}–{sfid_start+count-1}（8 bit），"
                           "各小帧载荷连续填充" if count > 1 else "；无ID副帧")
                self.frame_summary.set(
                    f"已启用：每小帧{frame_bytes} byte，前{sync_bits} bit为同步字"
                    f"0x{int(self.frame_sync_word.get().removeprefix('0x').removeprefix('0X'), 16):X}，"
                    f"每小帧载荷{frame_bytes * 8 - sync_bits - (8 if count > 1 else 0)} bit，帧率{frame_rate:.6g} frame/s"
                    f"（{frame_class}）{id_note}{pn_note}。")
            else:
                self.frame_summary.set(
                    "小帧同步未启用；启用后按“同步字 + PN/数据载荷”组成固定长度小帧。")
        except (OSError, ValueError):
            self.bit_summary.set("请完成数据源和有效BIT数设置。")
            self.bit_timing_summary.set(
                "请输入0.000001–61.44 Mbit/s，以计算样点/BIT和任务时长。")
            if self.frame_sync_enabled.get():
                self.frame_summary.set("小帧参数无效：请检查同步字长度、同步字和小帧长度。")

    def start_bit_transfer(self) -> None:
        if self.__dict__.get("network_busy", False):
            self._log("正在检测/重置连接，请完成后再发送BIT。")
            return
        if self.worker_active:
            messagebox.showinfo("任务进行中", "请等待当前下载完成。", parent=self)
            return
        try:
            pcm_selected, soqpsk_selected = self._bit_modulation_selection()
            standard_selected = pcm_selected or soqpsk_selected
            self._commit_mega_fields(
                require_rate=True,
                require_deviation=pcm_selected,
                require_offset=standard_selected)
            payload, bits = self._payload()
            task, rate, gap = self._task_values()
            repeat = self._repeat_value()
            validate_repeat_payload_size(len(payload), repeat)
            channel = CHANNEL_VALUE[self.bit_channel.get()]
            profile = PROFILE_VALUE[self.profile.get()]
            packets = make_download_packets(payload, valid_bits=bits, task_id=task,
                                            channel=channel, profile=profile,
                                            bit_rate=rate, repeat_count=repeat)
            start_packet = TxBitPacket(Opcode.START, task_id=task, channel=channel,
                                       profile=profile, total_bits=bits,
                                       repeat_count=repeat, bit_rate=rate)
            txcf_payload = self._txcf_packet()
            refclk_payload = self._refclk_packet()
            board, local = self._endpoint()
        except (OSError, ValueError) as exc:
            self._error("BIT任务参数错误", exc)
            return
        if pn_randomizer_conflict(self.source_type.get(), pcm_selected,
                                  soqpsk_selected, self.line_code.get()):
            self._error("数据源冲突", ValueError(
                "IRIG随机化器与PN15/PRBS15使用相同多项式，本工程禁止二者叠加；"
                "请选择BIN、HEX、PRBS7或其他数据源。"))
            return
        if pcm_selected and profile != 2:
            self._error("Profile不匹配", ValueError("选择PCM/FM（PL）模式时，Profile必须选择标准4阶Bessel连续码率"))
            return
        if soqpsk_selected and profile != 4:
            self._error("Profile不匹配", ValueError("选择SOQPSK-TG（PL）时，Profile必须选择SOQPSK-TG"))
            return
        if not standard_selected and profile not in (0, 1) and not messagebox.askyesno(
                "预留Profile", "当前选择的Profile不是PCM/FM，继续只会写入Profile字段，不会生成对应正式调制波形。\n\n是否继续？", parent=self):
            return
        modulation_payload = None
        modulation_name = ""
        if standard_selected:
            try:
                modulation_payload = (self._pcm_packet(True) if pcm_selected else
                                      self._soqpsk_packet(True))
            except ValueError as exc:
                self._error("调制参数错误", exc)
                return
            modulation_name = "PCM/FM" if pcm_selected else "SOQPSK-TG"
        self._update_summary(payload, bits)
        waveform_signature = self._current_waveform_signature()
        self.worker_active = True
        self._control_generation += 1
        self._set_operation_status("busy", "正在下发BIT并切换波形…")
        self.transfer_cancel.clear()
        self.send_button.configure(state="disabled")
        self.progress.set(0)
        self.transfer_phase.set("阶段1/3：正在停止旧任务并下载BIT")
        self._log(f"启动三阶段事务：STOP/CLEAR → BIT下载/COMMIT → 波形配置 → START；任务{task}；"
                  f"{bits} bit，{rate / 1e6:g} Mbit/s，{channel.name}，repeat={repeat}，"
                  f"任务包间隔={gap:g} ms。")
        self.transfer_thread = threading.Thread(target=self._transfer_worker,
                         args=(board, local, task, packets, start_packet,
                         txcf_payload, modulation_payload, modulation_name, gap,
                         refclk_payload, waveform_signature),
                         daemon=True)
        self.transfer_thread.start()

    def _transfer_worker(self, board: str, local: str, task: int,
                         packets: list[TxBitPacket], start_packet: TxBitPacket,
                         txcf_payload: bytes, modulation_payload: bytes | None,
                         modulation_name: str, gap_ms: float,
                         refclk_payload: bytes | None = None,
                         waveform_signature: tuple[str, ...] | None = None) -> None:
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                sock.bind((local, 0))
                if (self.__dict__.get("monitor_running", False) and
                        not self.monitor_stop.is_set()):
                    self.monitor_quiesced.clear()
                    self.monitor_pause.set()
                    sock.sendto(b"STR0", (board, CTRL_PORT))
                    if not self.monitor_quiesced.wait(timeout=1.0):
                        self.events.put((
                            "transfer_phase",
                            "监视暂停确认超时；已强制停止抓图，继续执行波形事务"))
                    time.sleep(MONITOR_QUIESCE_SETTLE_S)
                # A previous infinite task makes the firmware reject a new
                # BEGIN. STOP and CLEAR must be processed by the PS main loop
                # before BEGIN arrives; sending all datagrams back-to-back is
                # unsafe because the ISR only latches these two commands.
                send_bit_confirmed(sock, (board, CTRL_PORT),
                                   TxBitPacket(Opcode.STOP, task_id=task))
                time.sleep(TASK_RESET_SETTLE_S)
                if self.transfer_cancel.is_set():
                    self._cancel_rf_worker(board, local)
                    return
                send_bit_confirmed(sock, (board, CTRL_PORT),
                                   TxBitPacket(Opcode.CLEAR, task_id=task))
                wait_bit_state(sock, (board, CTRL_PORT), task)
                time.sleep(TASK_RESET_SETTLE_S)
                if refclk_payload is not None:
                    # The reference source must be changed while TX is still
                    # stopped.  The PS rebuilds the AD9361 clock chain here.
                    self.events.put(("reference_status", "正在等待板端参考时钟校验…"))
                    try:
                        detail = confirm_reference(sock, (board, CTRL_PORT), refclk_payload)
                    except (OSError, ValueError) as exc:
                        self.events.put(("reference_status", f"参考时钟未确认：{exc}"))
                        raise
                    self.events.put(("reference_status", detail))
                operation_count = len(packets) + 2 + int(refclk_payload is not None) + int(modulation_payload is not None)
                completed = 0
                for packet in packets:
                    if self.transfer_cancel.is_set():
                        self._cancel_rf_worker(board, local)
                        return
                    send_bit_confirmed(sock, (board, CTRL_PORT), packet)
                    completed += 1
                    self.events.put(("progress", completed * 100 / operation_count))
                    if gap_ms:
                        time.sleep(gap_ms / 1000)
                # COMMIT does not start DMA.  At this point every BIT datagram
                # has left the PC socket; allow the PS foreground loop to mark
                # the download committed before changing the waveform.
                self.events.put(("transfer_phase", "阶段1/3：板端已确认完整接收并COMMIT"))
                time.sleep(BIT_COMMIT_SETTLE_S)
                if self.transfer_cancel.is_set():
                    self._cancel_rf_worker(board, local)
                    return

                self.events.put(("transfer_phase", "阶段2/3：BIT已提交，正在切换射频与调制波形"))
                sock.sendto(txcf_payload, (board, CTRL_PORT))
                completed += 1
                self.events.put(("progress", completed * 100 / operation_count))
                time.sleep(MODE_CONFIG_SETTLE_S)
                if modulation_payload is not None:
                    if self.transfer_cancel.is_set():
                        self._cancel_rf_worker(board, local)
                        return
                    sock.sendto(modulation_payload, (board, CTRL_PORT))
                    completed += 1
                    self.events.put(("progress", completed * 100 / operation_count))
                    self.events.put(("transfer_phase", f"阶段2/3：{modulation_name}参数已下发，等待PL稳定"))
                    time.sleep(MODE_CONFIG_SETTLE_S)

                if self.transfer_cancel.is_set():
                    self._cancel_rf_worker(board, local)
                    return
                self.events.put(("transfer_phase", "阶段2/3：等待换频校准完成及板端配置回读"))
                confirm_rf_config(board, local, txcf_payload, port=CTRL_PORT,
                                  cancelled=self.transfer_cancel.is_set)
                self.events.put(("transfer_phase", "阶段3/3：板端RF配置已确认，正在发送START"))
                send_bit_confirmed(sock, (board, CTRL_PORT), start_packet)
                wait_bit_state(sock, (board, CTRL_PORT), task, started=True)
                completed += 1
                self.events.put(("progress", completed * 100 / operation_count))
                # BIT ACK/state and CFGQ have already been checked above.
                # Request extra serial diagnostics: the combined RF/LVDS
                # TXST report is more detailed than the UDP BIT status reply.
                if not self.transfer_cancel.is_set():
                    time.sleep(0.050)
                    sock.sendto(TxBitPacket(Opcode.STATUS, task_id=task).encode(),
                                (board, CTRL_PORT))
                    time.sleep(0.020)
                    sock.sendto(b"TXST", (board, CTRL_PORT))
        except (OSError, ValueError) as exc:
            self._abort_rf_worker(board, local)
            self.events.put(("transfer_cancelled", None) if self.transfer_cancel.is_set()
                            else ("transfer_error", str(exc)))
        else:
            self.events.put(("transfer_done", {
                "operation_count": operation_count,
                "waveform_signature": waveform_signature,
            }))

    def send_bit_command(self, opcode: Opcode, quiet: bool = False) -> None:
        if opcode in (Opcode.STOP, Opcode.CLEAR) and self.worker_active:
            self.transfer_cancel.set()
        try:
            task = int(self.task_id.get())
            packet = TxBitPacket(opcode, task_id=task).encode()
        except ValueError as exc:
            if not quiet:
                self._error("命令参数错误", exc)
            return
        if self.send_control(packet, f"BIT {opcode.name}，任务ID={task}") and opcode == Opcode.STATUS:
            self._log("提示：已请求BIT状态；完整BIT/RF诊断请查看Vitis串口，板级配置可通过自动回读核对。")

    def query_tx_runtime_status(self) -> None:
        """Request coherent BIT/DMA, PL-waveform and AD9361 RF diagnostics."""
        self.send_bit_command(Opcode.STATUS, quiet=True)
        time.sleep(0.020)
        if self.send_control(b"TXST", "已请求BIT/DMA、PL波形与AD9361射频门控联合状态"):
            self._log(
                "请在Vitis串口核对紧邻的[txb1] STATUS、[tx-check] BIT和"
                "[tx-check] RF-CFG/PWR状态；无限循环时应为active=1、busy=1、err=0，"
                "且RF、PWR均为PASS。")

    def _poll_events(self) -> None:
        latest_monitor_frame: object | None = None
        try:
            while True:
                kind, value = self.events.get_nowait()
                if kind == "board_config":
                    self.config_busy = False
                    config, error, token = value
                    if config is None:
                        self.config_status.set("当前配置：未读到，界面值未获板卡确认")
                        self._log(error)
                    else:
                        self._apply_board_configuration(config, token)
                elif kind == "temperature":
                    self.temperature_busy = False
                    temperature, error = value
                    if temperature is None:
                        self.temperature_text.set(f"AD9361温度：不可用（{error}）；非0℃")
                    else:
                        self.temperature_updated = time.monotonic()
                        self.temperature_text.set(
                            f"AD9361：{temperature:.1f} ℃ · 芯片传感器估算/未做单点校准 · "
                            f"{time.strftime('%H:%M:%S')}更新 · Zynq未接入")
                elif kind == "network_result":
                    self.network_busy = False
                    self.network_test_button.configure(state="normal")
                    self.network_reset_button.configure(state="normal")
                    if value["ok"]:
                        label = "重置完成，双向通信正常" if value["reset"] else "双向通信正常"
                        self._set_connection_status("ok", f"{label} · {value['elapsed']:.1f} ms")
                        self._log(f"{label}；已收到匹配的PONG，非仅sendto成功。"
                                  "发射执行状态仍请查询BIT/RF串口回读。")
                        self.after(50, self.refresh_board_configuration)
                    else:
                        self._set_connection_status("error", "应答超时/连接失败")
                        self._log(f"连接操作失败：{value['error']}")
                        if value["reset"]:
                            self._log("未确认板端重置成功。若PS完全失去执行能力，UDP重置也无法到达；"
                                      "需重新运行ELF或板卡复位。")
                elif kind == "tfs_result":
                    self.tfs_active = False
                    self.tfs_status.set(str(value))
                    for button in self.tfs_buttons:
                        button.configure(state="normal")
                    self._log(f"TFS：{value}")
                elif kind == "log":
                    self._log(str(value))
                elif kind == "progress":
                    self.progress.set(float(value))
                elif kind == "reference_status":
                    self.reference_status.set(str(value))
                    self._log(str(value))
                elif kind in ("rf_done", "rf_error"):
                    self.worker_active = False
                    self.monitor_pause.clear()
                    self.monitor_quiesced.clear()
                    self.send_button.configure(state="normal")
                    if kind == "rf_error":
                        self.monitor_start_pending = False
                        self.reference_status.set(f"参考时钟未确认：{value}")
                        self._set_operation_status("error", "配置失败，未继续发射")
                        self._error("时钟/射频配置失败", value)
                    elif isinstance(value, tuple):
                        self.applied_waveform_signature = value
                        self._notify_apply_complete()
                        if self.monitor_start_pending:
                            self.monitor_start_pending = False
                            self.after(150, self.start_loopback_monitor)
                    else:
                        self._set_operation_status("ok", "参考时钟获板端应答；可应用发射参数")
                elif kind == "transfer_phase":
                    self.transfer_phase.set(str(value))
                elif kind == "transfer_done":
                    self.worker_active = False
                    self.monitor_pause.clear()
                    self.monitor_quiesced.clear()
                    self.send_button.configure(state="normal")
                    self.transfer_phase.set("完成：板端已确认BIT提交和启动；射频状态请查询BIT/RF状态")
                    result = value if isinstance(value, dict) else {
                        "operation_count": value, "waveform_signature": None}
                    signature = result.get("waveform_signature")
                    if isinstance(signature, tuple):
                        self.applied_waveform_signature = signature
                    self._log(f"三阶段事务完成，共{result.get('operation_count')}个有效事务UDP包（不含STOP/CLEAR/STATUS）。"
                              "请在Vitis控制台核对START日志中的rate及reg回读值。")
                    self._notify_apply_complete()
                    if self.monitor_start_pending:
                        self.monitor_start_pending = False
                        self.after(150, self.start_loopback_monitor)
                elif kind == "transfer_error":
                    self.worker_active = False
                    self.monitor_pause.clear()
                    self.monitor_quiesced.clear()
                    self.monitor_start_pending = False
                    self.send_button.configure(state="normal")
                    self.transfer_phase.set("失败：BIT事务未完成")
                    self._set_operation_status("error", f"BIT事务失败：{value}")
                    self._error("BIT发送失败", value)
                elif kind == "transfer_cancelled":
                    self.worker_active = False
                    self.monitor_pause.clear()
                    self.monitor_quiesced.clear()
                    self.monitor_start_pending = False
                    self.send_button.configure(state="normal")
                    self.transfer_phase.set("已取消：未继续切换/启动新任务")
                    self._set_operation_status("stopped", "BIT事务已取消")
                    self._log("BIT任务已取消，旧下载不会在后台继续发送。")
                elif kind == "monitor_started":
                    detail = value if isinstance(value, dict) else {
                        "refresh_hz": value, "source": self.monitor_active_source,
                        "loopback": False}
                    self.loopback_status.set(
                        f"{detail['source']}已选择；DMA按完整帧抓取，"
                        f"目标刷新率{detail['refresh_hz']} frame/s。")
                    self.monitor_rx_lo_hz = detail.get("rx_lo_hz")
                    self.monitor_rx_bw_hz = detail.get("rx_bw_hz")
                    self._log(
                        f"{detail['source']}监视已启动；目标{detail['refresh_hz']} frame/s，"
                        f"{self.__dict__.get('monitor_capture_request', b'SNAP').decode('ascii')}单帧上传。"
                        "请在串口确认[tx-check] MON PASS。")
                elif kind == "monitor_frame":
                    # Drawing every queued frame can make Tk fall behind at
                    # 20 FPS.  Keep only the newest complete capture per poll.
                    latest_monitor_frame = value
                elif kind == "monitor_timeout":
                    if isinstance(value, dict):
                        self.loopback_status.set(
                            f"采集未收齐：本块{value['received']}/{value['expected'] or '?'}分片，"
                            f"累计收到{value['packets']}包、无效{value['invalid']}包；"
                            f"超时{value['incomplete']}次。"
                            "零包请检查UDP40001/本机IP/防火墙；有包不完整请检查丢包及固件版本。")
                        self.loopback_stats.set(
                            f"完整帧={value['completed']}/{value['requested']}，"
                            f"收到={value['packets']}包；未收到完整块，不绘制拼凑/补零波形。")
                    else:
                        self.loopback_status.set(f"等待完整采集帧超时（累计{value}帧）；正在自动重试。")
                elif kind == "monitor_error":
                    self.loopback_status.set(f"信号监视失败：{value}")
                    self._log(f"信号监视失败：{value}")
                elif kind == "monitor_stopped":
                    self.monitor_running = False
                    self.monitor_start_button.configure(state="normal")
                    self.monitor_stop_button.configure(state="disabled")
                    if not self.loopback_status.get().startswith("信号监视失败"):
                        self.loopback_status.set("I/Q监视已停止；已请求恢复默认RX配置。")
                    self._log("I/Q监视已停止并清除采集；TX DAC为默认监视源。")
        except queue.Empty:
            pass
        if latest_monitor_frame is not None:
            try:
                self._render_monitor_frame(latest_monitor_frame)
            except Exception as exc:
                # A Tk draw error must not permanently kill the event pump,
                # leaving networking alive with an apparently frozen plot.
                self.loopback_status.set(f"完整I/Q已收到，但绘图失败：{type(exc).__name__}: {exc}")
                self._log(self.loopback_status.get())
            finally:
                self.monitor_frame_pending.clear()
        self.after(25, self._poll_events)

    def _close(self) -> None:
        self.monitor_stop.set()
        self.transfer_cancel.set()
        try:
            with self._new_socket() as sock:
                sock.sendto(b"STR0", (self.board_ip.get().strip(), CTRL_PORT))
                sock.sendto(loopback_control_packet(False),
                            (self.board_ip.get().strip(), CTRL_PORT))
                if self.monitor_active_source == "RX ADC·外接SMA自收":
                    sock.sendto(rf_rx_control_packet(False),
                                (self.board_ip.get().strip(), CTRL_PORT))
                sock.sendto(monitor_source_packet(1),
                            (self.board_ip.get().strip(), CTRL_PORT))
        except (OSError, ValueError):
            pass
        self.destroy()


def main() -> int:
    """Start the UI and leave a readable diagnostic if startup ever fails."""
    if "--self-test" in sys.argv:
        # This path intentionally avoids creating a window.  Release builds run
        # it after extraction to prove that Python, Tcl/Tk, NumPy, Matplotlib,
        # local modules and the binary protocol were all bundled correctly.
        try:
            test_packet = TxBitPacket(
                Opcode.STATUS, task_id=1, channel=Channel.TX1).encode()
            if TxBitPacket.decode(test_packet).opcode is not Opcode.STATUS:
                raise RuntimeError("TX BIT protocol round-trip failed")
            if np is None or Figure is None or FigureCanvasTkAgg is None:
                raise RuntimeError("NumPy/Matplotlib/Tk backend is unavailable")
            root = tk.Tk()
            root.withdraw()
            root.update_idletasks()
            root.destroy()
            observation = analyze_monitor_iq(
                np.array([0.0, 1.0, 0.0, -1.0]),
                np.array([1.0, 0.0, -1.0, 0.0]), IQ_SAMPLE_RATE_HZ)
            if len(observation.spectrum_dbfs) != 4:
                raise RuntimeError("I/Q analysis self-test failed")
            result_dir = (Path(sys.executable).resolve().parent
                          if getattr(sys, "frozen", False) else UI_DIR)
            (result_dir / "portable_self_test.ok").write_text(
                f"{UI_BUILD}\nPython/Tk/NumPy/Matplotlib/protocol: PASS\n",
                encoding="utf-8")
            return 0
        except Exception:
            print(traceback.format_exc(), file=sys.stderr)
            return 2
    try:
        ControlUI().mainloop()
        return 0
    except Exception as exc:  # GUI startup failures otherwise vanish in --windowed builds.
        details = traceback.format_exc()
        try:
            log_dir = (Path(sys.executable).resolve().parent
                       if getattr(sys, "frozen", False) else UI_DIR)
            (log_dir / "ui_startup_error.log").write_text(details, encoding="utf-8")
        except OSError:
            pass
        try:
            root = tk.Tk()
            root.withdraw()
            messagebox.showerror(
                "AD9361TX UI 启动失败",
                f"{exc}\n\n详细信息已尝试写入 ui_startup_error.log。")
            root.destroy()
        except Exception:
            print(details, file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
