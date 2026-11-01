"""Uncorrected TX-DAC observations, not a symbol synchronizer/demodulator.

Keep the complex FFT, DC, image and spurs visible. Never project samples onto
an ideal constellation or normalize each point independently.
"""
from __future__ import annotations

from dataclasses import dataclass
import math

try:
    import numpy as np
except ImportError:
    np = None


@dataclass(frozen=True)
class SpectralPeak:
    frequency_hz: float
    level_dbfs: float


@dataclass(frozen=True)
class IQObservation:
    iq: object
    frequency_hz: object
    spectrum_dbfs: object
    points: object
    axis_limit: float
    peaks: tuple[SpectralPeak, ...]
    bin_hz: float


def estimate_occupied_bandwidth(observation: IQObservation):
    """Return baseband power centroid and approximate 99% occupied width.

    This FFT estimate removes a flat noise pedestal. It is a display aid,
    not a calibrated spectrum-analyzer or IRIG compliance measurement.
    """
    if np is None:
        raise RuntimeError("NumPy is required for I/Q analysis")
    levels = np.asarray(observation.spectrum_dbfs, dtype=np.float64)
    frequencies = np.asarray(observation.frequency_hz, dtype=np.float64)
    if levels.size < 4 or levels.shape != frequencies.shape:
        return None
    floor_db = float(np.percentile(levels, 20))
    # 单帧白噪声最高bin也可能比20百分位高10dB；不能据此伪报接收中心。
    # 以中位谱底判断显著谱结构，并拒绝几乎铺满采样带宽的噪声估计。
    if float(np.max(levels)) - float(np.median(levels)) < 12.0:
        return None
    power = np.power(10.0, levels / 10.0)
    power = np.maximum(power - 10.0 ** (floor_db / 10.0), 0.0)
    total = float(np.sum(power))
    if not math.isfinite(total) or total <= 0.0:
        return None
    cumulative = np.cumsum(power) / total
    lo = int(np.searchsorted(cumulative, 0.005))
    hi = min(len(frequencies) - 1, int(np.searchsorted(cumulative, 0.995)))
    centroid_hz = float(np.dot(frequencies, power) / total)
    width_hz = float(frequencies[hi] - frequencies[lo] + observation.bin_hz)
    if width_hz >= 0.90 * len(frequencies) * observation.bin_hz:
        return None
    return centroid_hz, width_hz


def analyze_monitor_iq(i, q, sample_rate_hz: float, full_scale: float = 2048.0):
    """One complete contiguous frame; i/q are aligned signed DAC codes.

    Tone amplitude uses the Hann coherent gain, so this is dBFS/bin (not
    dBm or a noise-density PSD). Peak frequencies are nearest FFT bins;
    neighbouring main-lobe bins are excluded, not presented as extra tones.
    """
    if np is None:
        raise RuntimeError("NumPy is required for I/Q analysis")
    if (not math.isfinite(sample_rate_hz) or sample_rate_hz <= 0 or
            not math.isfinite(full_scale) or full_scale <= 0):
        raise ValueError("Sample rate and full scale must be positive and finite")
    i = np.asarray(i, dtype=np.float64)
    q = np.asarray(q, dtype=np.float64)
    if (i.ndim != 1 or q.shape != i.shape or i.size < 4 or
            not np.all(np.isfinite(i)) or not np.all(np.isfinite(q))):
        raise ValueError("I/Q must have the same length (>=4), with finite samples")
    z = (i + 1j * q) / full_scale
    window = np.hanning(z.size)
    magnitude = np.abs(np.fft.fftshift(np.fft.fft(z * window))) / window.sum()
    dbfs = 20 * np.log10(np.maximum(magnitude, 1e-12))
    frequency = np.fft.fftshift(np.fft.fftfreq(z.size, 1 / sample_rate_hz))

    # Circular neighbours handle the negative Nyquist bin correctly.
    candidates = np.flatnonzero((magnitude > np.roll(magnitude, 1)) &
                               (magnitude >= np.roll(magnitude, -1)))
    selected: list[int] = []
    for index in candidates[np.argsort(magnitude[candidates])[::-1]]:
        if dbfs[index] < -110 or (selected and dbfs[index] < dbfs[selected[0]] - 90):
            break
        # Four-bin guard separates a tone from its Hann main lobe.
        if all(min(abs(index - prev), z.size - abs(index - prev)) > 4
               for prev in selected):
            selected.append(int(index))
        if len(selected) == 3:
            break
    peaks = tuple(SpectralPeak(float(frequency[k]), float(dbfs[k])) for k in selected)
    # No periodic display decimation: 4096 points are inexpensive and
    # decimating by a fixed stride can omit recurring phase states.
    points = np.column_stack((z.real, z.imag))
    peak_axis = float(np.max(np.abs(points)))
    axis_limit = max(0.02, math.ceil(peak_axis * 1.12 * 100) / 100)
    return IQObservation(z, frequency, dbfs, points, axis_limit, peaks,
                         sample_rate_hz / z.size)


def spectral_peak_summary(observation: IQObservation, *, cw: bool = False) -> str:
    peaks = observation.peaks
    if not peaks:
        return "未检出高于显示门限的谱峰。"
    main = peaks[0]
    text = f"主峰 {main.frequency_hz / 1e6:+.3f} MHz / {main.level_dbfs:.1f} dBFS"
    for peak in peaks[1:]:
        label = "次峰"
        if cw:
            if abs(peak.frequency_hz) <= observation.bin_hz:
                label = "DC候选"
            elif abs(peak.frequency_hz + main.frequency_hz) <= 2 * observation.bin_hz:
                label = "镜像候选"
            else:
                label = "杂散候选"
        text += (f"；{label} {peak.frequency_hz / 1e6:+.3f} MHz / "
                 f"{peak.level_dbfs - main.level_dbfs:.1f} dBc")
    return text + "（FFT栅格估计，不等同于谐波判定）。"
