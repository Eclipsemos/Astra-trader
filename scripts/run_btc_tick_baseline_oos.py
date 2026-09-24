#!/usr/bin/env python3
"""Evaluate a small BTC baseline on raw aggregate-trade samples and C++ replay tapes.

This is a diagnostic baseline, not a production model. It uses the same labels, chronological
splits, replay sequence mapping, costs, and decision tape contract as the Laya experiment. The
implementation intentionally uses only the Python standard library so a missing ML environment
cannot hide whether the features contain a directional signal.
"""

from __future__ import annotations

import argparse
import csv
import gzip
import hashlib
import json
import math
import subprocess
from collections import Counter
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

BASE_FEATURES = ("ret_1m", "ret_5m", "ret_15m", "ret_60m", "vol_60m", "flow_5m", "trades_5m", "spread_proxy")
FEATURES = BASE_FEATURES
SPLITS = ("development_train", "development_validation", "holdout_2026")
CLASS_NAMES = ("hold", "long", "short")
COSTS = {"base": (500, 100), "stress": (1_000, 500)}
SPLIT_ENDS = {
    "development_train": datetime(2025, 7, 1, tzinfo=UTC),
    "development_validation": datetime(2026, 1, 1, tzinfo=UTC),
    "holdout_2026": datetime(2026, 9, 1, tzinfo=UTC),
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_replay_sequences(path: Path) -> dict[int, int]:
    mapping: dict[int, int] = {}
    with path.open(newline="", encoding="utf-8") as handle:
        for sequence, row in enumerate(csv.DictReader(handle), 1):
            mapping[int(row["exchange_time_ns"])] = sequence
    return mapping


def load_replay_prices(path: Path) -> list[float]:
    prices: list[float] = []
    with path.open(newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            prices.append((int(row["best_bid_units"]) + int(row["best_ask_units"])) / 2)
    return prices


def sample_split(timestamp_ms: int) -> str | None:
    stamp = datetime.fromtimestamp(timestamp_ms / 1000, UTC)
    if datetime(2025, 1, 1, tzinfo=UTC) <= stamp < SPLIT_ENDS["development_train"]:
        return "development_train"
    if SPLIT_ENDS["development_train"] <= stamp < SPLIT_ENDS["development_validation"]:
        return "development_validation"
    if SPLIT_ENDS["development_validation"] <= stamp < SPLIT_ENDS["holdout_2026"]:
        return "holdout_2026"
    return None


def load_samples(path: Path, replay_sequences: dict[int, int]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    with gzip.open(path, "rt", newline="", encoding="utf-8") as handle:
        for record in csv.DictReader(handle):
            timestamp_ms = int(record["timestamp_ms"])
            split = sample_split(timestamp_ms)
            sequence = replay_sequences.get(timestamp_ms * 1_000_000)
            if split is None or sequence is None:
                continue
            rows.append(
                {
                    "split": split,
                    "sequence": sequence,
                    "features": [float(record[name]) for name in BASE_FEATURES],
                    "label": int(record["label"]),
                }
            )
    return rows


def standardizer(rows: list[dict[str, Any]]) -> tuple[list[float], list[float]]:
    means = [sum(row["features"][column] for row in rows) / len(rows) for column in range(len(FEATURES))]
    scales = []
    for column, mean in enumerate(means):
        variance = sum((row["features"][column] - mean) ** 2 for row in rows) / max(1, len(rows) - 1)
        scales.append(math.sqrt(variance) or 1.0)
    return means, scales


def values(row: dict[str, Any], means: list[float], scales: list[float]) -> list[float]:
    return [(value - mean) / scale for value, mean, scale in zip(row["features"], means, scales)] + [1.0]


def softmax(scores: list[float]) -> list[float]:
    top = max(scores)
    values_ = [math.exp(max(-60.0, score - top)) for score in scores]
    total = sum(values_)
    return [value / total for value in values_]


def train(rows: list[dict[str, Any]], means: list[float], scales: list[float]) -> list[list[float]]:
    weights = [[0.0] * (len(FEATURES) + 1) for _ in CLASS_NAMES]
    counts = Counter(int(row["label"]) for row in rows)
    class_weights = [min(5.0, max(0.5, len(rows) / (3.0 * max(1, counts[index])))) for index in range(3)]
    for epoch in range(160):
        rate = 0.035 / (1.0 + epoch / 80.0)
        for row in rows:
            vector = values(row, means, scales)
            prediction = softmax([sum(weight * value for weight, value in zip(parameters, vector)) for parameters in weights])
            label = int(row["label"])
            for class_index in range(3):
                error = class_weights[label] * ((1.0 if class_index == label else 0.0) - prediction[class_index])
                for feature_index, value in enumerate(vector):
                    penalty = 0.0005 * weights[class_index][feature_index] if feature_index < len(FEATURES) else 0.0
                    weights[class_index][feature_index] += rate * (error * value - penalty)
    return weights


def predict(rows: list[dict[str, Any]], weights: list[list[float]], means: list[float], scales: list[float]) -> list[list[float]]:
    return [softmax([sum(weight * value for weight, value in zip(parameters, values(row, means, scales))) for parameters in weights]) for row in rows]


def action_for(probability: list[float], threshold: float) -> int:
    direction = 1 if probability[1] >= probability[2] else 2
    return direction if probability[direction] >= threshold else 0


def relabel(rows: list[dict[str, Any]], prices: list[float], horizon: int, round_trip_cost_bps: float, margin_bps: float) -> None:
    cost = round_trip_cost_bps / 10_000
    margin = margin_bps / 10_000
    for row in rows:
        index = int(row["sequence"]) - 1
        if index + horizon >= len(prices):
            row["label"] = 0
            continue
        current = prices[index]
        future = prices[index + horizon]
        long_edge = future / current - 1.0 - cost
        short_edge = current / future - 1.0 - cost
        row["label"] = 1 if long_edge > margin and long_edge >= short_edge else 2 if short_edge > margin else 0


def add_directional_features(rows: list[dict[str, Any]], prices: list[float]) -> None:
    for row in rows:
        index = int(row["sequence"]) - 1
        def log_return(lookback: int) -> float:
            return math.log(prices[index] / prices[index - lookback]) if index >= lookback and prices[index - lookback] > 0 else 0.0

        def efficiency(lookback: int) -> float:
            if index < lookback or prices[index - lookback] <= 0:
                return 0.0
            numerator = abs(log_return(lookback))
            denominator = sum(abs(math.log(prices[offset] / prices[offset - 1])) for offset in range(index - lookback + 1, index + 1) if prices[offset - 1] > 0)
            return numerator / denominator if denominator else 0.0

        row["features"].extend((log_return(120), log_return(240), efficiency(60), efficiency(240)))


def proxy_score(rows: list[dict[str, Any]], probabilities: list[list[float]], threshold: float) -> tuple[float, int]:
    score = 0.0
    actions = 0
    for row, probability in zip(rows, probabilities):
        action = action_for(probability, threshold)
        if action:
            actions += 1
            score += 1.0 if action == int(row["label"]) else -1.0 if int(row["label"]) in (1, 2) else 0.0
    return score, actions


def prediction_report(rows: list[dict[str, Any]], probabilities: list[list[float]], threshold: float) -> tuple[dict[str, Any], list[int]]:
    actions = [action_for(probability, threshold) for probability in probabilities]
    correct = sum(action == int(row["label"]) for row, action in zip(rows, actions))
    return {
        "accuracy": round(correct / len(rows), 6) if rows else 0.0,
        "label_counts": dict(Counter(CLASS_NAMES[int(row["label"])] for row in rows)),
        "action_counts": dict(Counter(CLASS_NAMES[action] for action in actions)),
        "directional_actions": sum(action in (1, 2) for action in actions),
        "proxy_score": round(proxy_score(rows, probabilities, threshold)[0], 3),
        "log_loss": round(-sum(math.log(max(1e-12, p[int(row["label"])])) for row, p in zip(rows, probabilities)) / len(rows), 6) if rows else 0.0,
    }, actions


def write_tape(path: Path, rows: list[dict[str, Any]], probabilities: list[list[float]], threshold: float) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(("sequence", "action", "long_probability_ppm", "short_probability_ppm", "hold_probability_ppm"))
        for row, probability in zip(rows, probabilities):
            action = action_for(probability, threshold)
            writer.writerow((row["sequence"], CLASS_NAMES[action], round(probability[1] * 1_000_000), round(probability[2] * 1_000_000), round(probability[0] * 1_000_000)))


def run_cpp(binary: Path, input_path: Path, tape: Path, output: Path, start_ns: int, end_ns: int, fee: int, slippage: int) -> dict[str, Any]:
    subprocess.run(
        [str(binary), "--input", str(input_path), "--output", str(output), "--model", "tape", "--decision-tape", str(tape), "--decision-every", "60", "--probability-threshold-ppm", "0", "--start-ns", str(start_ns), "--end-ns", str(end_ns), "--taker-fee-ppm", str(fee), "--market-slippage-ppm", str(slippage)],
        check=True,
        capture_output=True,
        text=True,
    )
    return json.loads(output.read_text(encoding="utf-8"))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--samples", type=Path, default=Path("/home/ldtdev/qt/mmtick/data/order_flow_cache/btc-laya-1m-samples-v1.csv.gz"))
    parser.add_argument("--input", type=Path, default=Path("data/replay/btc-2025-2026.csv"))
    parser.add_argument("--binary", type=Path, default=Path("build/dev/astra_replay"))
    parser.add_argument("--output-dir", type=Path, default=Path("reports/btc_tick_baseline_oos"))
    parser.add_argument("--horizon-minutes", type=int, default=60)
    parser.add_argument("--round-trip-cost-bps", type=float, default=12.0)
    parser.add_argument("--label-margin-bps", type=float, default=2.0)
    parser.add_argument("--directional-features", action="store_true", help="add causal 120/240-minute returns and trend-efficiency features")
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    replay_sequences = load_replay_sequences(args.input)
    rows = load_samples(args.samples, replay_sequences)
    replay_prices = load_replay_prices(args.input)
    global FEATURES
    if args.directional_features:
        FEATURES = BASE_FEATURES + ("ret_120m", "ret_240m", "efficiency_60m", "efficiency_240m")
        add_directional_features(rows, replay_prices)
    if args.horizon_minutes != 60 or args.round_trip_cost_bps != 12.0 or args.label_margin_bps != 2.0:
        relabel(rows, replay_prices, args.horizon_minutes, args.round_trip_cost_bps, args.label_margin_bps)
    grouped = {split: [row for row in rows if row["split"] == split] for split in SPLITS}
    means, scales = standardizer(grouped["development_train"])
    weights = train(grouped["development_train"], means, scales)
    probabilities = {split: predict(grouped[split], weights, means, scales) for split in SPLITS}
    candidates = []
    for threshold in (0.34, 0.36, 0.38, 0.40, 0.42, 0.45, 0.50, 0.55, 0.60):
        score, actions = proxy_score(grouped["development_validation"], probabilities["development_validation"], threshold)
        candidates.append({"threshold": threshold, "proxy_score": round(score, 3), "directional_actions": actions})
    selected = max(candidates, key=lambda item: (item["proxy_score"], item["directional_actions"] if item["proxy_score"] > 0 else -item["directional_actions"], item["threshold"]))
    threshold = float(selected["threshold"])
    tape_paths = {}
    cpp = {}
    periods = {
        "development_train": (1735689600000000000, 1751327999000000000),
        "development_validation": (1751328000000000000, 1767225599000000000),
        "holdout_2026": (1767225600000000000, 1788220799000000000),
    }
    for split in SPLITS:
        tape = args.output_dir / f"{split}-baseline-tape.csv"
        write_tape(tape, grouped[split], probabilities[split], threshold)
        tape_paths[split] = {"path": str(tape), "sha256": sha256(tape), "bytes": tape.stat().st_size}
        cpp[split] = {}
        for cost_name, (fee, slippage) in COSTS.items():
            output = args.output_dir / f"{split}-cpp-{cost_name}.json"
            cpp[split][cost_name] = run_cpp(args.binary, args.input, tape, output, *periods[split], fee, slippage)
    report = {
        "schema": "astra.report.btc-tick-baseline-oos.v1",
        "generated_at": datetime.now(UTC).isoformat(),
        "input": {"path": str(args.input), "sha256": sha256(args.input), "samples_path": str(args.samples), "samples_sha256": sha256(args.samples)},
        "protocol": {"features": FEATURES, "samples": {split: len(grouped[split]) for split in SPLITS}, "train_only_standardization": True, "model": "class-weighted softmax baseline", "directional_features": args.directional_features, "label_horizon_minutes": args.horizon_minutes, "label_round_trip_cost_bps": args.round_trip_cost_bps, "label_margin_bps": args.label_margin_bps, "cpp_probability_threshold_ppm": 0},
        "calibration": {"selected_threshold": threshold, "candidates": candidates},
        "prediction": {split: prediction_report(grouped[split], probabilities[split], threshold)[0] for split in SPLITS},
        "decision_tapes": tape_paths,
        "cpp_tape_evaluation": cpp,
        "decision": {"paper_approved": False, "reason": "Diagnostic baseline only; positive replay evidence and rolling OOS gates are required before paper use."},
    }
    (args.output_dir / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"threshold": threshold, "samples": report["protocol"]["samples"], "prediction": report["prediction"], "cpp_fills": {split: {cost: cpp[split][cost]["fills"] for cost in COSTS} for split in SPLITS}}, indent=2))


if __name__ == "__main__":
    main()
