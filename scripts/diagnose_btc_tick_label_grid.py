#!/usr/bin/env python3
"""Diagnose BTC sample label balance across horizons and executable cost assumptions."""

from __future__ import annotations

import argparse
import csv
import gzip
import hashlib
import json
import math
from collections import Counter, defaultdict
from datetime import UTC, datetime
from pathlib import Path
from statistics import mean, pstdev

FEATURES = ("ret_1m", "ret_5m", "ret_15m", "ret_60m", "vol_60m", "flow_5m", "trades_5m", "spread_proxy")
SPLITS = ("development_train", "development_validation", "holdout_2026")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def split(timestamp_ms: int) -> str | None:
    stamp = datetime.fromtimestamp(timestamp_ms / 1000, UTC)
    if datetime(2025, 1, 1, tzinfo=UTC) <= stamp < datetime(2025, 7, 1, tzinfo=UTC):
        return "development_train"
    if datetime(2025, 7, 1, tzinfo=UTC) <= stamp < datetime(2026, 1, 1, tzinfo=UTC):
        return "development_validation"
    if datetime(2026, 1, 1, tzinfo=UTC) <= stamp < datetime(2026, 9, 1, tzinfo=UTC):
        return "holdout_2026"
    return None


def load_replay(path: Path) -> tuple[dict[int, int], list[float]]:
    timestamps: dict[int, int] = {}
    prices: list[float] = []
    with path.open(newline="", encoding="utf-8") as handle:
        for sequence, row in enumerate(csv.DictReader(handle), 1):
            timestamps[int(row["exchange_time_ns"])] = sequence - 1
            prices.append((int(row["best_bid_units"]) + int(row["best_ask_units"])) / 2)
    return timestamps, prices


def load_samples(path: Path, timestamp_to_index: dict[int, int]) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    with gzip.open(path, "rt", newline="", encoding="utf-8") as handle:
        for record in csv.DictReader(handle):
            timestamp_ms = int(record["timestamp_ms"])
            period = split(timestamp_ms)
            index = timestamp_to_index.get(timestamp_ms * 1_000_000)
            if period is None or index is None:
                continue
            rows.append({"split": period, "index": index, "features": [float(record[name]) for name in FEATURES]})
    return rows


def label(long_edge: float, short_edge: float, margin: float) -> int:
    if max(long_edge, short_edge) <= margin:
        return 0
    return 1 if long_edge >= short_edge else 2


def effect_sizes(rows: list[dict[str, object]], labels: list[int]) -> dict[str, dict[str, float]]:
    output: dict[str, dict[str, float]] = {}
    for feature_index, feature in enumerate(FEATURES):
        groups = {kind: [float(row["features"][feature_index]) for row, kind in zip(rows, labels) if kind == label_value] for kind, label_value in (("hold", 0), ("long", 1), ("short", 2))}
        pooled = pstdev([float(row["features"][feature_index]) for row in rows]) or 1.0
        output[feature] = {
            "train_mean": round(mean(float(row["features"][feature_index]) for row in rows), 10),
            "train_std": round(pooled, 10),
            "long_minus_short_std": round((mean(groups["long"]) - mean(groups["short"])) / pooled, 6) if groups["long"] and groups["short"] else 0.0,
            "long_minus_hold_std": round((mean(groups["long"]) - mean(groups["hold"])) / pooled, 6) if groups["long"] and groups["hold"] else 0.0,
            "short_minus_hold_std": round((mean(groups["short"]) - mean(groups["hold"])) / pooled, 6) if groups["short"] and groups["hold"] else 0.0,
        }
    return output


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--samples", type=Path, default=Path("/home/ldtdev/qt/mmtick/data/order_flow_cache/btc-laya-1m-samples-v1.csv.gz"))
    parser.add_argument("--input", type=Path, default=Path("data/replay/btc-2025-2026.csv"))
    parser.add_argument("--output", type=Path, default=Path("reports/btc_tick_label_grid/results.json"))
    args = parser.parse_args()
    timestamp_to_index, prices = load_replay(args.input)
    rows = load_samples(args.samples, timestamp_to_index)
    by_split = {name: [row for row in rows if row["split"] == name] for name in SPLITS}
    grid: list[dict[str, object]] = []
    for horizon in (15, 30, 60, 120, 240):
        for cost_bps in (8, 12, 20, 30):
            for margin_bps in (0, 2, 5):
                split_counts = {}
                for split_name, split_rows in by_split.items():
                    counts: Counter[str] = Counter()
                    for row in split_rows:
                        index = int(row["index"])
                        if index + horizon >= len(prices):
                            continue
                        current = prices[index]
                        future = prices[index + horizon]
                        cost = 2.0 * cost_bps / 10_000
                        long_edge = future / current - 1.0 - cost
                        short_edge = current / future - 1.0 - cost
                        counts[("hold", "long", "short")[label(long_edge, short_edge, margin_bps / 10_000)]] += 1
                    total = sum(counts.values())
                    split_counts[split_name] = {"hold": counts["hold"], "long": counts["long"], "short": counts["short"], "directional_fraction": round((counts["long"] + counts["short"]) / total, 6) if total else 0.0}
                grid.append({"horizon_minutes": horizon, "round_trip_cost_bps": cost_bps, "margin_bps": margin_bps, "splits": split_counts})
    train_rows = by_split["development_train"]
    labels_60 = []
    for row in train_rows:
        index = int(row["index"])
        future = prices[index + 60]
        current = prices[index]
        labels_60.append(label(future / current - 1.0 - 0.0024, current / future - 1.0 - 0.0024, 0.0002))
    report = {
        "schema": "astra.report.btc-tick-label-grid.v1",
        "generated_at": datetime.now(UTC).isoformat(),
        "input": {"replay_path": str(args.input), "replay_sha256": sha256(args.input), "samples_path": str(args.samples), "samples_sha256": sha256(args.samples)},
        "sample_counts": {split_name: len(split_rows) for split_name, split_rows in by_split.items()},
        "feature_effects_train_at_60m": effect_sizes(train_rows, labels_60),
        "grid": grid,
        "decision": "diagnostic only; no parameter set is paper-approved",
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"sample_counts": report["sample_counts"], "grid_rows": len(grid), "feature_effects_train_at_60m": report["feature_effects_train_at_60m"]}, indent=2))


if __name__ == "__main__":
    main()
