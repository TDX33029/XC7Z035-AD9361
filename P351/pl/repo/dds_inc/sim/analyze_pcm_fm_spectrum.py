"""IRIG-106 PCM/FM digital occupied-bandwidth pre-check."""
from pathlib import Path
import sys
import numpy as np

FS_HZ = 61_440_000.0
def main(path: str, bit_rate_hz: float = 12_500_000.0) -> int:
    samples = np.loadtxt(Path(path))
    if samples.ndim != 2 or samples.shape[1] < 2 or len(samples) < 65536:
        raise ValueError("capture must contain at least 65536 signed I/Q rows")
    signal = samples[:, 0] + 1j * samples[:, 1]
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
    expected_b99_hz = 1.16 * bit_rate_hz
    bandwidth_ok = abs(bandwidth - expected_b99_hz) <= 0.12 * expected_b99_hz
    envelope_ok = ripple_pct <= 2.0
    print(f"PCM/FM {bit_rate_hz/1e6:g}M B99={bandwidth/1e6:.6f} MHz "
          f"(IRIG estimate {expected_b99_hz/1e6:.3f} MHz)")
    print(f"B99 edges={low/1e6:.6f}/{high/1e6:.6f} MHz")
    print(f"constant-envelope RMS ripple={ripple_pct:.3f}%")
    print("PASS digital PCM/FM pre-check" if bandwidth_ok and envelope_ok
          else "FAIL digital PCM/FM pre-check")
    return 0 if bandwidth_ok and envelope_ok else 1


if __name__ == "__main__":
    if len(sys.argv) not in (2, 3):
        raise SystemExit("usage: analyze_pcm_fm_spectrum.py <iq-text-file> [bit-rate-hz]")
    raise SystemExit(main(sys.argv[1], float(sys.argv[2]) if len(sys.argv) == 3 else 12_500_000.0))
