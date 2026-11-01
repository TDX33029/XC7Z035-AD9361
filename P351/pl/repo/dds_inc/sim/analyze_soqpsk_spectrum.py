"""Digital pre-check for an RTL SOQPSK-TG capture.

This checks occupied bandwidth and constant envelope at the PL I/Q output.
It does not replace the IRIG-106 Eq. 2-27 RF mask measurement after AD9361.
"""
from pathlib import Path
import sys

import numpy as np


FS_HZ = 61_440_000.0
def main(path: str, bit_rate_hz: float = 12_500_000.0) -> int:
    samples = np.loadtxt(path)
    if samples.ndim != 2 or samples.shape[1] != 2 or len(samples) < 32768:
        raise ValueError("capture must contain at least 32768 signed I/Q rows")
    signal = samples[:, 0] + 1j * samples[:, 1]
    # The DC component is part of the captured signal. Removing its mean
    # alters both occupied power and the envelope of a constant-envelope CPM.
    window = np.hanning(len(signal))
    spectrum = np.fft.fftshift(np.fft.fft(signal * window))
    frequency = np.fft.fftshift(np.fft.fftfreq(len(signal), 1.0 / FS_HZ))
    power = np.abs(spectrum) ** 2
    power /= np.sum(power)
    cumulative = np.cumsum(power)
    low = frequency[np.searchsorted(cumulative, 0.005)]
    high = frequency[np.searchsorted(cumulative, 0.995)]
    bandwidth = high - low
    envelope = np.abs(signal)
    ripple_pct = 100.0 * np.std(envelope) / np.mean(envelope)
    expected_b99_hz = 0.78 * bit_rate_hz
    bandwidth_ok = abs(bandwidth - expected_b99_hz) <= 0.10 * expected_b99_hz
    envelope_ok = ripple_pct <= 2.0
    print(f"SOQPSK {bit_rate_hz/1e6:g}M B99={bandwidth/1e6:.6f} MHz "
          f"(expected about {expected_b99_hz/1e6:.3f} MHz)")
    print(f"B99 edges={low/1e6:.6f}/{high/1e6:.6f} MHz")
    print(f"constant-envelope RMS ripple={ripple_pct:.3f}%")
    print("PASS digital spectrum pre-check" if bandwidth_ok and envelope_ok
          else "FAIL digital spectrum pre-check")
    return 0 if bandwidth_ok and envelope_ok else 1


if __name__ == "__main__":
    if len(sys.argv) not in (2, 3):
        raise SystemExit("usage: analyze_soqpsk_spectrum.py <iq-text-file> [bit-rate-hz]")
    raise SystemExit(main(sys.argv[1], float(sys.argv[2]) if len(sys.argv) == 3 else 12_500_000.0))
