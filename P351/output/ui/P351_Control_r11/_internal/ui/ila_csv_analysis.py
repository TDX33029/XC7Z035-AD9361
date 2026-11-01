"""Read-only analysis of Vivado ILA CSV captures for the TX BIT path."""
from __future__ import annotations

import csv
import math
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path


UINT32_MASK = (1 << 32) - 1
PCM_LINE_CODES = ("NRZ-L", "NRZ-M", "NRZ-S", "RNRZ-L", "Biφ-L", "Biφ-M", "Biφ-S")
LINE_CODE_ALIASES = {
    "Biφ-L（Manchester）": "Biφ-L",
    "RNRZ-L（IRIG随机化）": "RNRZ-L",
}


@dataclass(frozen=True)
class IlaBitAnalysis:
    csv_path: Path
    sample_rate_hz: float
    sample_count: int
    accepted_events: int
    first_event_sample: int | None
    last_event_sample: int | None
    elapsed_cycles: int | None
    measured_bit_rate: float | None
    measured_rate_low: float | None
    measured_rate_high: float | None
    rate_word: int | None
    rate_word_bit_rate: float | None
    interval_counts: dict[int, int] = field(default_factory=dict)
    requested_bit_rate: int | None = None
    compared_bits: int = 0
    bit_errors: int | None = None
    ber: float | None = None
    mismatch_samples: tuple[int, ...] = ()
    mismatch_bits_sent: tuple[int, ...] = ()
    counter_continuity: bool | None = None
    accepted_counter_continuity: bool | None = None
    underrun_samples: int | None = None
    engine_error_samples: int | None = None
    bit_ready_low_samples: int | None = None
    pcm_valid_low_samples: int | None = None
    line_code: str | None = None
    encoding_compared_bits: int = 0
    encoding_errors: int | None = None
    encoding_passed: bool | None = None
    encoding_incomplete_bits: int = 0
    encoding_error_positions: tuple[int, ...] = ()
    warnings: tuple[str, ...] = ()


def _column(headers: list[str], suffix: str, required: bool = True) -> int | None:
    for index, header in enumerate(headers):
        if header.strip() == suffix or header.strip().endswith(suffix):
            return index
    if required:
        raise ValueError(f"ILA CSV缺少必要探针列：{suffix}")
    return None


def _parse_int(text: str, radix: str, default_base: int) -> int:
    value = text.strip()
    if not value:
        raise ValueError("ILA CSV包含空数据单元")
    if value.lower().startswith("0x"):
        return int(value, 16)
    radix_upper = radix.strip().upper()
    if radix_upper == "HEX":
        return int(value, 16)
    if radix_upper in ("SIGNED", "UNSIGNED"):
        return int(value, 10)
    return int(value, default_base)


def _reference_bit(payload: bytes, valid_bits: int, bit_index: int) -> int:
    index = bit_index % valid_bits
    return (payload[index // 8] >> (7 - index % 8)) & 1


def _normalize_line_code(line_code: str | None) -> str | None:
    if line_code is None:
        return None
    normalized = LINE_CODE_ALIASES.get(line_code.strip(), line_code.strip())
    if normalized not in PCM_LINE_CODES:
        raise ValueError(f"不支持的PCM码型：{line_code}")
    return normalized


def _continuity(values: list[int], frame_end: list[bool] | None = None) -> bool:
    for index, (previous, current) in enumerate(zip(values, values[1:])):
        if ((current - previous) & UINT32_MASK) == 1:
            continue
        if current == 0 and frame_end is not None and frame_end[index]:
            continue
        return False
    return True


def analyze_ila_csv(
    csv_path: str | Path,
    *,
    sample_rate_hz: float = 61_440_000.0,
    reference_payload: bytes | None = None,
    reference_valid_bits: int | None = None,
    requested_bit_rate: int | None = None,
    line_code: str | None = None,
) -> IlaBitAnalysis:
    """Analyze accepted BIT events and optional exact BER from a Vivado CSV.

    All fields remain strings until parsed with the radix row.  This is
    important for counters such as ``00002e10``, which spreadsheet readers
    can otherwise reinterpret as decimal or scientific notation.
    """
    path = Path(csv_path)
    if not path.is_file():
        raise ValueError(f"CSV文件不存在：{path}")
    if sample_rate_hz <= 0:
        raise ValueError("ILA采样率必须大于0")
    normalized_line_code = _normalize_line_code(line_code)
    if reference_payload is not None:
        if not reference_payload:
            raise ValueError("参考BIT数据为空")
        if reference_valid_bits is None:
            reference_valid_bits = len(reference_payload) * 8
        if not 0 < reference_valid_bits <= len(reference_payload) * 8:
            raise ValueError("参考有效BIT数超出参考数据长度")

    with path.open("r", newline="", encoding="utf-8-sig") as handle:
        reader = csv.reader(handle)
        try:
            headers = next(reader)
        except StopIteration as exc:
            raise ValueError("ILA CSV为空") from exc
        try:
            second_row = next(reader)
        except StopIteration as exc:
            raise ValueError("ILA CSV没有数据行") from exc
        if second_row and second_row[0].strip().lower().startswith("radix"):
            radixes = second_row
            rows = list(reader)
        else:
            radixes = [""] * len(headers)
            rows = [second_row, *reader]

    rows = [row for row in rows if row and any(cell.strip() for cell in row)]
    if not rows:
        raise ValueError("ILA CSV没有有效采样数据")
    for row in rows:
        if len(row) < len(headers):
            row.extend([""] * (len(headers) - len(row)))

    sample_col = _column(headers, "Sample in Buffer", required=False)
    if sample_col is None:
        sample_col = _column(headers, "Sample in Window", required=False)
    bit_data_col = _column(headers, "tx_bit_engine_0_bit_data")
    bit_valid_col = _column(headers, "tx_bit_engine_0_bit_valid")
    bit_ready_col = _column(headers, "dds_inc_0_bit_ready", required=False)
    bits_sent_col = _column(headers, "tx_bit_engine_0_bits_sent[31:0]", required=False)
    bit_phase_col = _column(headers, "tx_bit_engine_0_bit_phase[31:0]", required=False)
    frame_end_col = _column(headers, "tx_bit_engine_0_frame_end", required=False)
    accepted_col = _column(headers, "dds_inc_0_pcm_accepted_bits[31:0]", required=False)
    underrun_col = _column(headers, "tx_bit_engine_0_underrun", required=False)
    error_col = _column(headers, "tx_bit_engine_0_error", required=False)
    pcm_valid_col = _column(headers, "dds_inc_0_pcm_iq_valid", required=False)
    pcm_nrz_col = _column(headers, "dds_inc_0_pcm_debug_nrz[15:0]", required=False)
    if pcm_nrz_col is None:
        pcm_nrz_col = _column(headers, "dds_inc_0_pcm_debug_nrz", required=False)

    def value(row: list[str], column: int, default_base: int = 16) -> int:
        radix = radixes[column] if column < len(radixes) else ""
        return _parse_int(row[column], radix, default_base)

    sample_numbers = [value(row, sample_col, 10) if sample_col is not None else index
                      for index, row in enumerate(rows)]
    bit_valid = [value(row, bit_valid_col) != 0 for row in rows]
    bit_ready = ([value(row, bit_ready_col) != 0 for row in rows]
                 if bit_ready_col is not None else [True] * len(rows))
    event_rows = [index for index, (valid, ready) in enumerate(zip(bit_valid, bit_ready))
                  if valid and ready]
    event_samples = [sample_numbers[index] for index in event_rows]
    interval_counts = Counter(current - previous
                              for previous, current in zip(event_samples, event_samples[1:]))

    measured_rate = None
    measured_low = None
    measured_high = None
    elapsed_cycles = None
    if len(event_samples) >= 2:
        elapsed_cycles = event_samples[-1] - event_samples[0]
        if elapsed_cycles <= 0:
            raise ValueError("ILA采样序号不是严格递增")
        intervals = len(event_samples) - 1
        measured_rate = intervals * sample_rate_hz / elapsed_cycles
        measured_low = intervals * sample_rate_hz / (elapsed_cycles + 1)
        if elapsed_cycles > 1:
            measured_high = intervals * sample_rate_hz / (elapsed_cycles - 1)

    rate_word = None
    rate_word_rate = None
    phases: list[int] | None = None
    if bit_phase_col is not None and len(rows) >= 2:
        phases = [value(row, bit_phase_col) & UINT32_MASK for row in rows]
        phase_deltas = [((current - previous) & UINT32_MASK)
                        for previous, current in zip(phases, phases[1:])]
        nonzero_deltas = [delta for delta in phase_deltas if delta]
        if nonzero_deltas:
            rate_word = Counter(nonzero_deltas).most_common(1)[0][0]
            rate_word_rate = rate_word * sample_rate_hz / (1 << 32)

    event_bits_sent: list[int] = []
    frame_end_events: list[bool] | None = None
    counter_continuity = None
    if bits_sent_col is not None:
        event_bits_sent = [value(rows[index], bits_sent_col) & UINT32_MASK
                           for index in event_rows]
        if frame_end_col is not None:
            frame_end_events = [value(rows[index], frame_end_col) != 0
                                for index in event_rows]
        counter_continuity = _continuity(event_bits_sent, frame_end_events)

    accepted_continuity = None
    if accepted_col is not None:
        accepted_values = [value(rows[index], accepted_col) & UINT32_MASK
                           for index in event_rows]
        accepted_continuity = _continuity(accepted_values)

    bit_errors = None
    ber = None
    mismatch_samples: list[int] = []
    mismatch_bits_sent: list[int] = []
    compared_bits = 0
    warnings: list[str] = []
    if reference_payload is not None:
        if bits_sent_col is None:
            warnings.append("CSV没有bits_sent探针，无法把抓取BIT与参考数据对齐。")
        else:
            observed = [value(rows[index], bit_data_col) & 1 for index in event_rows]
            for row_index, sample, sent, actual in zip(
                    event_rows, event_samples, event_bits_sent, observed):
                del row_index
                expected = _reference_bit(reference_payload, reference_valid_bits, sent)
                if actual != expected:
                    mismatch_samples.append(sample)
                    mismatch_bits_sent.append(sent)
            compared_bits = len(observed)
            bit_errors = len(mismatch_samples)
            ber = bit_errors / compared_bits if compared_bits else math.nan
    else:
        warnings.append("未提供参考BIT数据，只计算码率和链路状态，不计算BER。")

    encoding_compared = 0
    encoding_errors: int | None = None
    encoding_passed: bool | None = None
    encoding_incomplete = 0
    encoding_error_positions: list[int] = []
    if normalized_line_code is not None:
        if pcm_nrz_col is None:
            warnings.append("CSV没有pcm_debug_nrz探针，无法判断PCM码型。")
        elif len(event_rows) < 2:
            warnings.append("PCM码型判断至少需要两个有效BIT事件。")
        else:
            event_input_bits = [value(rows[index], bit_data_col) & 1
                                for index in event_rows]

            def majority_level(indices: range | list[int]) -> int | None:
                levels: list[int] = []
                for row_index in indices:
                    raw = value(rows[row_index], pcm_nrz_col) & 0xFFFF
                    if raw == 0:
                        continue
                    levels.append(0 if raw & 0x8000 else 1)
                if not levels:
                    return None
                zeros = levels.count(0)
                ones = len(levels) - zeros
                return None if zeros == ones else int(ones > zeros)

            # Each accepted BIT becomes visible in pcm_debug_nrz from the next
            # ILA sample.  The following accepted event closes that BIT window.
            # phase[31] selects the second half.  If phase is absent, use the
            # window midpoint as a lower-confidence fallback.
            records: list[tuple[int, int | None, int | None, int]] = []
            for event_index in range(len(event_rows) - 1):
                start = event_rows[event_index] + 1
                stop = event_rows[event_index + 1]
                if stop <= start:
                    records.append((event_input_bits[event_index], None, None,
                                    event_index))
                    continue
                if phases is not None:
                    first_indices = [index for index in range(start, stop)
                                     if not (phases[index] & 0x80000000)]
                    second_indices = [index for index in range(start, stop)
                                      if phases[index] & 0x80000000]
                else:
                    midpoint = start + (stop - start) // 2
                    first_indices = list(range(start, midpoint))
                    second_indices = list(range(midpoint, stop))
                position = (event_bits_sent[event_index]
                            if event_bits_sent else event_index)
                records.append((event_input_bits[event_index],
                                majority_level(first_indices),
                                majority_level(second_indices), position))

            encoding_incomplete = sum(first is None or second is None
                                      for _, first, second, _ in records)
            error_positions: set[int] = set()
            previous_end: int | None = None
            outputs: list[int | None] = []
            for index, (input_bit, first, second, position) in enumerate(records):
                if first is None or second is None:
                    outputs.append(None)
                    previous_end = None
                    continue
                bit_error = False
                if normalized_line_code == "NRZ-L":
                    bit_error = first != input_bit or second != input_bit
                elif normalized_line_code == "NRZ-M":
                    bit_error = first != second
                    if previous_end is not None:
                        bit_error |= first != (previous_end ^ input_bit)
                elif normalized_line_code == "NRZ-S":
                    bit_error = first != second
                    if previous_end is not None:
                        bit_error |= first != (previous_end ^ (1 - input_bit))
                elif normalized_line_code == "Biφ-L":
                    bit_error = first != input_bit or second != (1 - input_bit)
                elif normalized_line_code == "Biφ-M":
                    bit_error = second == first
                    if previous_end is not None:
                        bit_error |= first != (previous_end ^ (1 - input_bit))
                elif normalized_line_code == "Biφ-S":
                    bit_error = second == first
                    if previous_end is not None:
                        bit_error |= first != (previous_end ^ input_bit)
                else:  # RNRZ-L: y[n] = x[n] xor y[n-14] xor y[n-15].
                    bit_error = first != second
                    if index >= 15 and outputs[index - 14] is not None and outputs[index - 15] is not None:
                        expected = input_bit ^ outputs[index - 14] ^ outputs[index - 15]
                        bit_error |= first != expected
                    elif index < 15:
                        # Initial randomizer state may predate an arbitrary ILA
                        # window, so only the equal-half NRZ-L rule is testable.
                        pass
                outputs.append(first)
                previous_end = second
                encoding_compared += 1
                if bit_error:
                    error_positions.add(position)

            encoding_errors = len(error_positions)
            encoding_error_positions = sorted(error_positions)
            minimum = 16 if normalized_line_code == "RNRZ-L" else 8
            if encoding_compared >= minimum:
                encoding_passed = encoding_errors == 0
            else:
                warnings.append(
                    f"{normalized_line_code}码型仅得到{encoding_compared}个完整BIT，"
                    f"至少需要{minimum}个才能判定。")
            if phases is None:
                warnings.append("CSV没有bit_phase，码型分析按BIT窗口中点分半；建议补抓bit_phase提高可信度。")

    if len(event_samples) < 2:
        warnings.append("有效BIT事件少于2个，无法从事件间隔计算实测码率。")
    if rate_word is None:
        warnings.append("CSV没有可用bit_phase，无法计算Q0.32长期平均码率。")

    def count_nonzero(column: int | None) -> int | None:
        if column is None:
            return None
        return sum(value(row, column) != 0 for row in rows)

    return IlaBitAnalysis(
        csv_path=path,
        sample_rate_hz=sample_rate_hz,
        sample_count=len(rows),
        accepted_events=len(event_rows),
        first_event_sample=event_samples[0] if event_samples else None,
        last_event_sample=event_samples[-1] if event_samples else None,
        elapsed_cycles=elapsed_cycles,
        measured_bit_rate=measured_rate,
        measured_rate_low=measured_low,
        measured_rate_high=measured_high,
        rate_word=rate_word,
        rate_word_bit_rate=rate_word_rate,
        interval_counts=dict(sorted(interval_counts.items())),
        requested_bit_rate=requested_bit_rate,
        compared_bits=compared_bits,
        bit_errors=bit_errors,
        ber=ber,
        mismatch_samples=tuple(mismatch_samples),
        mismatch_bits_sent=tuple(mismatch_bits_sent),
        counter_continuity=counter_continuity,
        accepted_counter_continuity=accepted_continuity,
        underrun_samples=count_nonzero(underrun_col),
        engine_error_samples=count_nonzero(error_col),
        bit_ready_low_samples=(sum(not ready for ready in bit_ready)
                               if bit_ready_col is not None else None),
        pcm_valid_low_samples=(sum(value(row, pcm_valid_col) == 0 for row in rows)
                               if pcm_valid_col is not None else None),
        line_code=normalized_line_code,
        encoding_compared_bits=encoding_compared,
        encoding_errors=encoding_errors,
        encoding_passed=encoding_passed,
        encoding_incomplete_bits=encoding_incomplete,
        encoding_error_positions=tuple(encoding_error_positions),
        warnings=tuple(warnings),
    )


def format_ila_analysis(result: IlaBitAnalysis) -> str:
    """Format an operator-facing Chinese report for the UI result pane."""
    lines = [
        "ILA BIT率 / BER分析结果",
        f"CSV：{result.csv_path}",
        f"采样率：{result.sample_rate_hz / 1e6:.6f} MHz",
        f"采样点：{result.sample_count:,}；有效BIT握手：{result.accepted_events:,}",
    ]
    if result.measured_bit_rate is not None:
        lines.append(
            f"短窗口实测BIT率：{result.measured_bit_rate:,.6f} bit/s "
            f"({result.measured_bit_rate / 1e6:.9f} Mbit/s)")
        if result.measured_rate_low is not None and result.measured_rate_high is not None:
            lines.append(
                "首末采样±1周期范围："
                f"{result.measured_rate_low:,.3f}～{result.measured_rate_high:,.3f} bit/s")
    if result.rate_word is not None and result.rate_word_bit_rate is not None:
        lines.append(
            f"Q0.32步进字：0x{result.rate_word:08X}；长期平均BIT率："
            f"{result.rate_word_bit_rate:,.9f} bit/s")
    if result.requested_bit_rate and result.rate_word_bit_rate is not None:
        error = result.rate_word_bit_rate - result.requested_bit_rate
        ppm = error / result.requested_bit_rate * 1e6
        lines.append(
            f"相对UI目标{result.requested_bit_rate:,} bit/s："
            f"{error:+.9f} bit/s（{ppm:+.6f} ppm）")
    if result.interval_counts:
        total = sum(result.interval_counts.values())
        interval_text = "，".join(
            f"{cycles}周期×{count}次({count / total * 100:.2f}%)"
            for cycles, count in result.interval_counts.items())
        lines.append(f"BIT间隔分布：{interval_text}")

    if result.bit_errors is not None and result.ber is not None:
        lines.append(
            f"BIN/当前数据源对比：{result.bit_errors}/{result.compared_bits} bit错误；"
            f"BER={result.ber:.12g}（{result.ber * 100:.6f}%）")
        if result.mismatch_bits_sent:
            shown = ", ".join(str(value) for value in result.mismatch_bits_sent[:20])
            suffix = " …" if len(result.mismatch_bits_sent) > 20 else ""
            lines.append(f"错误bits_sent位置（前20个）：{shown}{suffix}")

    if result.line_code is not None:
        if result.encoding_errors is None:
            lines.append(f"PCM码型判定：不可用（目标码型={result.line_code}）")
        else:
            decision = ("通过" if result.encoding_passed else
                        "失败" if result.encoding_passed is False else "样本不足")
            lines.append(
                f"PCM码型判定：{decision}；码型={result.line_code}；"
                f"完整BIT={result.encoding_compared_bits}；"
                f"错误BIT={result.encoding_errors}；不完整BIT={result.encoding_incomplete_bits}")
            if result.encoding_error_positions:
                shown = ", ".join(str(value) for value in result.encoding_error_positions[:20])
                suffix = " …" if len(result.encoding_error_positions) > 20 else ""
                lines.append(f"码型错误位置（前20个）：{shown}{suffix}")

    def status(label: str, value: bool | None) -> None:
        if value is not None:
            lines.append(f"{label}：{'通过' if value else '失败'}")

    status("bits_sent连续性", result.counter_continuity)
    status("调制器接收计数连续性", result.accepted_counter_continuity)
    for label, value in (
        ("underrun有效采样", result.underrun_samples),
        ("engine error有效采样", result.engine_error_samples),
        ("bit_ready低电平采样", result.bit_ready_low_samples),
        ("PCM IQ无效采样", result.pcm_valid_low_samples),
    ):
        if value is not None:
            lines.append(f"{label}：{value}")
    if result.warnings:
        lines.append("注意：")
        lines.extend(f"- {warning}" for warning in result.warnings)
    return "\n".join(lines)
