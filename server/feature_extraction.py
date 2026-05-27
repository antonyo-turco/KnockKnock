import math
from typing import Dict, List, Tuple

import numpy as np


def _safe_float(value: float) -> float:
    if value is None:
        return 0.0
    if isinstance(value, (float, int)):
        if math.isnan(value) or math.isinf(value):
            return 0.0
        return float(value)
    return 0.0


def _bandpower(freqs: np.ndarray, power: np.ndarray, f_low: float, f_high: float) -> float:
    mask = (freqs >= f_low) & (freqs < f_high)
    if not np.any(mask):
        return 0.0
    return _safe_float(np.trapz(power[mask], freqs[mask]))


def compute_time_features(values: List[float]) -> Dict[str, float]:
    arr = np.asarray(values, dtype=float)
    if arr.size == 0:
        return {
            "mean": 0.0,
            "std": 0.0,
            "rms": 0.0,
            "min": 0.0,
            "max": 0.0,
            "p95_abs": 0.0,
            "p99_abs": 0.0,
            "jerk_rms": 0.0,
            "jerk_max": 0.0,
            "crest_factor": 0.0,
        }

    abs_arr = np.abs(arr)
    rms = _safe_float(np.sqrt(np.mean(arr ** 2)))
    diffs = np.diff(arr) if arr.size > 1 else np.array([0.0])
    jerk_rms = _safe_float(np.sqrt(np.mean(diffs ** 2)))
    jerk_max = _safe_float(np.max(np.abs(diffs)))

    max_abs = _safe_float(np.max(abs_arr))
    crest_factor = _safe_float(max_abs / rms) if rms > 1e-12 else 0.0

    return {
        "mean": _safe_float(np.mean(arr)),
        "std": _safe_float(np.std(arr)),
        "rms": rms,
        "min": _safe_float(np.min(arr)),
        "max": _safe_float(np.max(arr)),
        "p95_abs": _safe_float(np.percentile(abs_arr, 95)),
        "p99_abs": _safe_float(np.percentile(abs_arr, 99)),
        "jerk_rms": jerk_rms,
        "jerk_max": jerk_max,
        "crest_factor": crest_factor,
    }


def _spectral_flux(signal: np.ndarray) -> float:
    if signal.size < 16:
        return 0.0
    mid = signal.size // 2
    first = signal[:mid]
    second = signal[mid:]
    n = min(first.size, second.size)
    if n < 8:
        return 0.0

    w = np.hanning(n)
    s1 = np.abs(np.fft.rfft((first[:n] - np.mean(first[:n])) * w))
    s2 = np.abs(np.fft.rfft((second[:n] - np.mean(second[:n])) * w))

    s1_sum = np.sum(s1)
    s2_sum = np.sum(s2)
    if s1_sum <= 1e-12 or s2_sum <= 1e-12:
        return 0.0

    s1 = s1 / s1_sum
    s2 = s2 / s2_sum
    return _safe_float(np.sqrt(np.mean((s2 - s1) ** 2)))


def compute_fft_features(values: List[float], sampling_rate_hz: float, n_fft: int = 512) -> Dict[str, float]:
    arr = np.asarray(values, dtype=float)
    if arr.size == 0:
        return {
            "dominant_freq_1": 0.0,
            "dominant_freq_2": 0.0,
            "bandpower_10_20": 0.0,
            "bandpower_20_40": 0.0,
            "spectral_entropy": 0.0,
            "spectral_flux": 0.0,
            "total_power": 0.0,
        }

    if sampling_rate_hz <= 0:
        sampling_rate_hz = 100.0

    if arr.size < n_fft:
        arr = np.pad(arr, (n_fft - arr.size, 0), mode="edge")
    elif arr.size > n_fft:
        arr = arr[-n_fft:]

    arr = arr - np.mean(arr)
    win = np.hanning(n_fft)
    spec = np.fft.rfft(arr * win)
    power = np.abs(spec) ** 2
    freqs = np.fft.rfftfreq(n_fft, d=1.0 / sampling_rate_hz)

    total_power = _safe_float(np.sum(power))
    if total_power <= 1e-12:
        dominant_freq_1 = 0.0
        dominant_freq_2 = 0.0
        spectral_entropy = 0.0
    else:
        p = power.copy()
        p[0] = 0.0
        idx1 = int(np.argmax(p))
        dominant_freq_1 = _safe_float(freqs[idx1])

        p[idx1] = 0.0
        idx2 = int(np.argmax(p))
        dominant_freq_2 = _safe_float(freqs[idx2])

        prob = power / np.sum(power)
        prob = np.clip(prob, 1e-12, 1.0)
        spectral_entropy = _safe_float(-np.sum(prob * np.log(prob)) / np.log(prob.size))

    return {
        "dominant_freq_1": dominant_freq_1,
        "dominant_freq_2": dominant_freq_2,
        "bandpower_10_20": _bandpower(freqs, power, 10.0, 20.0),
        "bandpower_20_40": _bandpower(freqs, power, 20.0, 40.0),
        "spectral_entropy": spectral_entropy,
        "spectral_flux": _spectral_flux(arr),
        "total_power": total_power,
    }


def compute_axis_features(values: List[float], sampling_rate_hz: float, n_fft: int = 512) -> Dict[str, float]:
    out = {}
    out.update(compute_time_features(values))
    out.update(compute_fft_features(values, sampling_rate_hz, n_fft=n_fft))
    return out


def compute_batch_features(
    x_values: List[float],
    y_values: List[float],
    z_values: List[float],
    sampling_rate_hz: float,
    n_fft: int = 512,
) -> Tuple[Dict[str, Dict[str, float]], float, int]:
    m_values = np.sqrt(np.asarray(x_values) ** 2 + np.asarray(y_values) ** 2 + np.asarray(z_values) ** 2).tolist()

    features = {
        "x": compute_axis_features(x_values, sampling_rate_hz, n_fft=n_fft),
        "y": compute_axis_features(y_values, sampling_rate_hz, n_fft=n_fft),
        "z": compute_axis_features(z_values, sampling_rate_hz, n_fft=n_fft),
        "m": compute_axis_features(m_values, sampling_rate_hz, n_fft=n_fft),
    }

    m_rms = features["m"]["rms"]
    m_p99 = features["m"]["p99_abs"]
    m_jerk = features["m"]["jerk_max"]
    m_band = features["m"]["bandpower_20_40"]
    m_flux = features["m"]["spectral_flux"]
    m_tot_pow = features["m"]["total_power"]

    p99_norm = min(1.0, m_p99 / (m_rms * 8.0 + 1e-9))
    jerk_norm = min(1.0, m_jerk / (m_rms * 12.0 + 1e-9))
    band_norm = min(1.0, m_band / (m_tot_pow * 0.3 + 1e-9))
    flux_norm = min(1.0, m_flux / 0.25)

    impact_score = _safe_float(0.35 * p99_norm + 0.30 * jerk_norm + 0.20 * band_norm + 0.15 * flux_norm)
    event_flag = 1 if impact_score >= 0.60 else 0

    return features, impact_score, event_flag
