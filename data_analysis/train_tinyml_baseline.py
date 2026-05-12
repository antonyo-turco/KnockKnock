#!/usr/bin/env python3
"""Train a tiny baseline detector from exported accelerometer features.

The current dataset contains only baseline samples (event_flag == 0), so this
script learns a compact baseline envelope instead of a supervised vibration
classifier. The generated artifact is intentionally ESP32-friendly: a handful of
threshold comparisons plus a small violation counter.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Dict, List, Sequence

FEATURE_COLUMNS = [
    "impact_score",
    "m_p99",
    "m_jerk_max",
    "m_band_20_40",
    "m_spectral_flux",
]

DEFAULT_INPUT = Path("exports/features.csv")
DEFAULT_OUTPUT_DIR = Path("tinyml_model")


@dataclass
class FeatureStats:
    feature: str
    count: int
    mean: float
    std: float
    median: float
    mad: float
    min: float
    p95: float
    p99: float
    p995: float
    max: float
    threshold: float


def _safe_float(value: float) -> float:
    if value is None:
        return 0.0
    if isinstance(value, (int, float)):
        if math.isnan(value) or math.isinf(value):
            return 0.0
        return float(value)
    return 0.0


def _percentile(values: Sequence[float], percentile: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    if len(ordered) == 1:
        return float(ordered[0])
    rank = (len(ordered) - 1) * (percentile / 100.0)
    lower = int(math.floor(rank))
    upper = int(math.ceil(rank))
    if lower == upper:
        return float(ordered[lower])
    weight = rank - lower
    return float(ordered[lower] * (1.0 - weight) + ordered[upper] * weight)


def _median(values: Sequence[float]) -> float:
    return _percentile(values, 50.0)


def _mad(values: Sequence[float], center: float | None = None) -> float:
    if not values:
        return 0.0
    if center is None:
        center = _median(values)
    deviations = [abs(v - center) for v in values]
    return _median(deviations)


def _load_rows(csv_path: Path) -> List[dict]:
    with csv_path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def _extract_feature_values(rows: Sequence[dict], feature: str) -> List[float]:
    values: List[float] = []
    for row in rows:
        raw = row.get(feature, "")
        if raw == "" or raw is None:
            continue
        try:
            values.append(float(raw))
        except ValueError:
            continue
    return values


def _build_stats(feature: str, values: Sequence[float]) -> FeatureStats:
    if not values:
        return FeatureStats(
            feature=feature,
            count=0,
            mean=0.0,
            std=0.0,
            median=0.0,
            mad=0.0,
            min=0.0,
            p95=0.0,
            p99=0.0,
            p995=0.0,
            max=0.0,
            threshold=0.0,
        )

    mean = sum(values) / len(values)
    variance = sum((value - mean) ** 2 for value in values) / len(values)
    std = math.sqrt(variance)
    median = _median(values)
    mad = _mad(values, median)
    p95 = _percentile(values, 95.0)
    p99 = _percentile(values, 99.0)
    p995 = _percentile(values, 99.5)
    upper_envelope = max(p995, median + 6.0 * mad, mean + 4.0 * std)
    threshold = upper_envelope * 1.05 if upper_envelope > 0 else 0.0

    return FeatureStats(
        feature=feature,
        count=len(values),
        mean=_safe_float(mean),
        std=_safe_float(std),
        median=_safe_float(median),
        mad=_safe_float(mad),
        min=_safe_float(min(values)),
        p95=_safe_float(p95),
        p99=_safe_float(p99),
        p995=_safe_float(p995),
        max=_safe_float(max(values)),
        threshold=_safe_float(threshold),
    )


def _score_row(row: dict, stats_map: Dict[str, FeatureStats]) -> float:
    # Simple one-class detector score: count and magnitude of threshold breaches.
    score = 0.0
    for feature, stats in stats_map.items():
        raw = row.get(feature, "")
        if raw in ("", None):
            continue
        try:
            value = float(raw)
        except ValueError:
            continue
        if value > stats.threshold:
            denom = stats.threshold if stats.threshold > 1e-12 else 1.0
            score += (value - stats.threshold) / denom
    return score


def _render_header(stats: Sequence[FeatureStats], allowed_violations: int, feature_count: int) -> str:
    lines = [
        "#pragma once",
        "",
        "#include <math.h>",
        "",
        "// Auto-generated from data_analysis/train_tinyml_baseline.py",
        "// Baseline detector based on exported feature statistics.",
        "",
        f"#define TINYML_BASELINE_FEATURE_COUNT {feature_count}",
        f"#define TINYML_BASELINE_ALLOWED_VIOLATIONS {allowed_violations}",
        "",
    ]

    for stat in stats:
        macro = stat.feature.upper()
        lines.append(f"#define TINYML_{macro}_THRESHOLD {stat.threshold:.9f}f")
    lines.append("")
    lines.append("static inline int tinyml_is_baseline_sample(")
    params = [f"    float {stat.feature}" for stat in stats]
    lines.append(",\n".join(params) + ") {")
    lines.append("    int violations = 0;")
    for stat in stats:
        macro = stat.feature.upper()
        lines.append(f"    if ({stat.feature} > TINYML_{macro}_THRESHOLD) {{")
        lines.append("        violations++;")
        lines.append("    }")
    lines.append("")
    lines.append("    return violations <= TINYML_BASELINE_ALLOWED_VIOLATIONS;")
    lines.append("}")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description="Train a tiny baseline detector from exported features.")
    parser.add_argument("--input-csv", type=Path, default=DEFAULT_INPUT, help="Path to exports/features.csv")
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT_DIR, help="Directory for generated artifacts")
    parser.add_argument("--allowed-violations", type=int, default=1, help="How many threshold breaches still count as baseline")
    args = parser.parse_args()

    rows = _load_rows(args.input_csv)
    if not rows:
        raise SystemExit(f"No rows found in {args.input_csv}")

    baseline_rows = [row for row in rows if str(row.get("event_flag", "0")) == "0"]
    vibration_rows = [row for row in rows if str(row.get("event_flag", "0")) != "0"]

    if not baseline_rows:
        raise SystemExit("No baseline rows found (event_flag == 0).")

    stats: List[FeatureStats] = []
    stats_map: Dict[str, FeatureStats] = {}
    for feature in FEATURE_COLUMNS:
        values = _extract_feature_values(baseline_rows, feature)
        stat = _build_stats(feature, values)
        stats.append(stat)
        stats_map[feature] = stat

    baseline_scores = [_score_row(row, stats_map) for row in baseline_rows]
    vibration_scores = [_score_row(row, stats_map) for row in vibration_rows]

    score_threshold = _percentile(baseline_scores, 99.5) + 0.05 if baseline_scores else 0.05
    max_baseline_score = max(baseline_scores) if baseline_scores else 0.0

    artifact = {
        "source_csv": str(args.input_csv),
        "rows_total": len(rows),
        "rows_baseline": len(baseline_rows),
        "rows_vibration": len(vibration_rows),
        "feature_columns": FEATURE_COLUMNS,
        "allowed_violations": args.allowed_violations,
        "score_threshold": _safe_float(score_threshold),
        "max_baseline_score": _safe_float(max_baseline_score),
        "stats": [asdict(stat) for stat in stats],
        "notes": [
            "Current dataset contains only baseline examples when event_flag == 0.",
            "This artifact is a baseline envelope, not yet a supervised vibration classifier.",
            "Add labeled vibration windows later to train a small decision tree or random forest.",
        ],
    }

    args.output_dir.mkdir(parents=True, exist_ok=True)
    model_json = args.output_dir / "tinyml_baseline_model.json"
    model_h = args.output_dir / "tinyml_baseline_model.h"
    report_md = args.output_dir / "README.md"

    model_json.write_text(json.dumps(artifact, indent=2), encoding="utf-8")
    model_h.write_text(_render_header(stats, args.allowed_violations, len(FEATURE_COLUMNS)), encoding="utf-8")
    report_md.write_text(
        "# TinyML Baseline Model\n\n"
        "This directory contains the first baseline detector derived from exported accelerometer features.\n\n"
        "Artifacts:\n"
        "- `tinyml_baseline_model.json`\n"
        "- `tinyml_baseline_model.h`\n\n"
        "The model currently learns only the baseline envelope because the exported dataset does not yet\n"
        "contain labeled vibration examples.\n",
        encoding="utf-8",
    )

    print("TinyML baseline detector trained")
    print(f"Input rows: {len(rows)}")
    print(f"Baseline rows: {len(baseline_rows)}")
    print(f"Vibration rows: {len(vibration_rows)}")
    print(f"Score threshold: {score_threshold:.6f}")
    print(f"Max baseline score: {max_baseline_score:.6f}")
    print(f"Artifacts written to: {args.output_dir}")

    for stat in stats:
        print(
            f"- {stat.feature}: threshold={stat.threshold:.6f}, mean={stat.mean:.6f}, std={stat.std:.6f}, "
            f"p99.5={stat.p995:.6f}"
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
