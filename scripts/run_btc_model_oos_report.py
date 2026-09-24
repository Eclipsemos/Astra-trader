#!/usr/bin/env python3
"""Train and evaluate a small BTC-specific cost-aware action model.

The script intentionally uses only the Python standard library so the research protocol can be
reproduced on the Astra host without installing a numerical stack.  It is not a production
inference implementation: the model is a compact linear softmax classifier whose purpose is to
answer whether BTC-specific historical calibration adds evidence beyond the frozen generic Laya
checkpoint.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
from array import array
from collections import Counter
from datetime import UTC, datetime
from pathlib import Path
from typing import Any, Iterable

SPLITS = {
    "development_train": ("2025-01-01T00:00:00+00:00", "2025-06-30T23:59:59+00:00"),
    "development_validation": ("2025-07-01T00:00:00+00:00", "2025-12-31T23:59:59+00:00"),
    "holdout_2026": ("2026-01-01T00:00:00+00:00", "2026-08-31T23:59:59+00:00"),
}
COSTS = {"base": (500, 100), "stress": (1_000, 500)}
CLASS_NAMES = ("hold", "long", "short")
FEATURE_NAMES = (
    "ret_1m",
    "ret_5m",
    "ret_15m",
    "ret_60m",
    "vol_60m",
    "flow_imbalance",
    "spread_bps",
    "hour_sin",
    "hour_cos",
)


def to_ns(value: str) -> int:
    return int(datetime.fromisoformat(value).timestamp() * 1_000_000_000)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_csv(path: Path) -> tuple[array, array, array, array, array, array]:
    timestamps = array("q")
    bids = array("q")
    asks = array("q")
    bid_qty = array("q")
    ask_qty = array("q")
    mids = array("q")
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        for row in reader:
            bid = int(row["best_bid_units"])
            ask = int(row["best_ask_units"])
            timestamps.append(int(row["exchange_time_ns"]))
            bids.append(bid)
            asks.append(ask)
            bid_qty.append(int(row["bid_quantity_units"]))
            ask_qty.append(int(row["ask_quantity_units"]))
            mids.append((bid + ask) // 2)
    if not timestamps:
        raise ValueError(f"input has no rows: {path}")
    return timestamps, bids, asks, bid_qty, ask_qty, mids


def log_return(mids: array, current: int, lookback: int) -> float:
    previous = int(mids[current - lookback])
    now = int(mids[current])
    return math.log(now / previous) if previous > 0 and now > 0 else 0.0


def rolling_volatility(mids: array, current: int, lookback: int) -> float:
    returns = [log_return(mids, current - offset, 1) for offset in range(lookback - 1, -1, -1)]
    if len(returns) < 2:
        return 0.0
    mean = sum(returns) / len(returns)
    return math.sqrt(sum((value - mean) ** 2 for value in returns) / (len(returns) - 1))


def feature_vector(
    index: int,
    timestamps: array,
    bids: array,
    asks: array,
    bid_qty: array,
    ask_qty: array,
    mids: array,
) -> list[float]:
    total_qty = int(bid_qty[index]) + int(ask_qty[index])
    flow = (int(bid_qty[index]) - int(ask_qty[index])) / total_qty if total_qty else 0.0
    mid = int(mids[index])
    spread_bps = (int(asks[index]) - int(bids[index])) / mid * 10_000 if mid else 0.0
    stamp = datetime.fromtimestamp(int(timestamps[index]) / 1_000_000_000, UTC)
    phase = 2.0 * math.pi * (stamp.hour + stamp.minute / 60.0) / 24.0
    return [
        log_return(mids, index, 1),
        log_return(mids, index, 5),
        log_return(mids, index, 15),
        log_return(mids, index, 60),
        rolling_volatility(mids, index, 60),
        flow,
        spread_bps,
        math.sin(phase),
        math.cos(phase),
    ]


def label_for(
    index: int,
    horizon: int,
    bids: array,
    asks: array,
    margin: float,
    fee_ppm: int,
    slippage_ppm: int,
) -> int:
    current_bid = int(bids[index])
    current_ask = int(asks[index])
    future_bid = int(bids[index + horizon])
    future_ask = int(asks[index + horizon])
    round_trip_cost = 2.0 * (fee_ppm + slippage_ppm) / 1_000_000.0
    long_edge = future_bid / current_ask - 1.0 - round_trip_cost
    short_edge = current_bid / future_ask - 1.0 - round_trip_cost
    if max(long_edge, short_edge) <= margin:
        return 0
    return 1 if long_edge >= short_edge else 2


def split_for(timestamp: int) -> str | None:
    for name, (start, end) in SPLITS.items():
        if to_ns(start) <= timestamp <= to_ns(end):
            return name
    return None


def build_samples(
    timestamps: array,
    bids: array,
    asks: array,
    bid_qty: array,
    ask_qty: array,
    mids: array,
    stride: int,
    horizon: int,
    label_margin: float,
) -> list[dict[str, Any]]:
    samples: list[dict[str, Any]] = []
    for index in range(60, len(timestamps) - horizon, stride):
        split = split_for(int(timestamps[index]))
        if split is None:
            continue
        samples.append(
            {
                "index": index,
                "timestamp_ns": int(timestamps[index]),
                "split": split,
                "features": feature_vector(index, timestamps, bids, asks, bid_qty, ask_qty, mids),
                "label": label_for(index, horizon, bids, asks, label_margin, *COSTS["base"]),
            }
        )
    return samples


def standardizer(samples: Iterable[dict[str, Any]]) -> tuple[list[float], list[float]]:
    rows = list(samples)
    count = len(rows)
    means = [sum(row["features"][column] for row in rows) / count for column in range(len(FEATURE_NAMES))]
    scales = []
    for column, mean in enumerate(means):
        variance = sum((row["features"][column] - mean) ** 2 for row in rows) / max(1, count - 1)
        scales.append(math.sqrt(variance) or 1.0)
    return means, scales


def transform(row: dict[str, Any], means: list[float], scales: list[float]) -> list[float]:
    return [(value - mean) / scale for value, mean, scale in zip(row["features"], means, scales)]


def softmax(scores: list[float], temperature: float = 1.0) -> list[float]:
    scaled = [score / temperature for score in scores]
    top = max(scaled)
    values = [math.exp(max(-60.0, value - top)) for value in scaled]
    total = sum(values)
    return [value / total for value in values]


def train_softmax(rows: list[dict[str, Any]], means: list[float], scales: list[float]) -> list[list[float]]:
    feature_count = len(FEATURE_NAMES)
    weights = [[0.0] * (feature_count + 1) for _ in CLASS_NAMES]
    labels = [int(row["label"]) for row in rows]
    counts = Counter(labels)
    total = len(rows)
    class_weights = [min(5.0, max(0.5, total / (3.0 * max(1, counts[index])))) for index in range(3)]
    learning_rate = 0.04
    l2 = 0.0005
    for epoch in range(140):
        rate = learning_rate / (1.0 + epoch / 80.0)
        for row in rows:
            values = transform(row, means, scales) + [1.0]
            scores = [sum(weight * value for weight, value in zip(parameters, values)) for parameters in weights]
            probabilities = softmax(scores)
            label = int(row["label"])
            weight_scale = class_weights[label]
            for class_index in range(3):
                error = weight_scale * ((1.0 if class_index == label else 0.0) - probabilities[class_index])
                for feature_index, value in enumerate(values):
                    penalty = l2 * weights[class_index][feature_index] if feature_index < feature_count else 0.0
                    weights[class_index][feature_index] += rate * (error * value - penalty)
    return weights


def probabilities(row: dict[str, Any], weights: list[list[float]], means: list[float], scales: list[float], temperature: float) -> list[float]:
    values = transform(row, means, scales) + [1.0]
    scores = [sum(weight * value for weight, value in zip(parameters, values)) for parameters in weights]
    return softmax(scores, temperature)


def log_loss(rows: list[dict[str, Any]], predictions: list[list[float]]) -> float:
    if not rows:
        return 0.0
    return -sum(math.log(max(1e-12, prediction[int(row["label"])])) for row, prediction in zip(rows, predictions)) / len(rows)


def brier_score(rows: list[dict[str, Any]], predictions: list[list[float]]) -> float:
    if not rows:
        return 0.0
    return sum(
        sum((probability - (1.0 if class_index == int(row["label"]) else 0.0)) ** 2 for class_index, probability in enumerate(prediction))
        for row, prediction in zip(rows, predictions)
    ) / len(rows)


def fill_price(price: int, side: int, slippage_ppm: int) -> int:
    if side > 0:
        return price + (price * slippage_ppm + 999_999) // 1_000_000
    return max(1, price - (price * slippage_ppm) // 1_000_000)


def simulate(
    rows: list[dict[str, Any]],
    actions: list[int],
    bids: array,
    asks: array,
    mids: array,
    fee_ppm: int,
    slippage_ppm: int,
) -> dict[str, Any]:
    initial_cash = 10_000_000
    cash = initial_cash
    position = 0
    average_entry = 0
    pending: int | None = None
    fills = 0
    fees = 0
    gross_profit = 0
    gross_loss = 0
    equity_peak = initial_cash
    max_drawdown = 0
    target_size = 100

    def execute(target: int, index: int) -> None:
        nonlocal cash, position, average_entry, fills, fees, gross_profit, gross_loss
        delta = target - position
        if delta == 0:
            return
        side = 1 if delta > 0 else -1
        quantity = abs(delta)
        price = fill_price(int(asks[index] if side > 0 else bids[index]), side, slippage_ppm)
        notional = price * quantity // 1_000
        fee = (notional * fee_ppm + 999_999) // 1_000_000
        cash += -notional - fee if side > 0 else notional - fee
        fees += fee
        remaining = quantity
        if position > 0 and side < 0:
            closing = min(position, remaining)
            pnl = (price - average_entry) * closing // 1_000
            gross_profit += max(0, pnl)
            gross_loss += max(0, -pnl)
            remaining -= closing
        elif position < 0 and side > 0:
            closing = min(-position, remaining)
            pnl = (average_entry - price) * closing // 1_000
            gross_profit += max(0, pnl)
            gross_loss += max(0, -pnl)
            remaining -= closing
        if remaining and target:
            average_entry = price
        elif target == 0:
            average_entry = 0
        elif position and ((position > 0) == (target > 0)) and abs(target) > abs(position):
            average_entry = (average_entry * abs(position) + price * remaining) // abs(target)
        position = target
        fills += 1

    equity_curve: list[int] = []
    for row, action in zip(rows, actions):
        index = int(row["index"])
        if pending is not None:
            execute(pending * target_size, index)
            pending = None
        pending = action if action in (1, -1) else 0
        equity = cash + position * int(mids[index]) // 1_000
        equity_curve.append(equity)
        equity_peak = max(equity_peak, equity)
        max_drawdown = max(max_drawdown, equity_peak - equity)
    final_equity = equity_curve[-1] if equity_curve else initial_cash
    return {
        "samples": len(rows),
        "fills": fills,
        "return_ppm": round((final_equity - initial_cash) * 1_000_000 / initial_cash),
        "final_equity_units": final_equity,
        "max_drawdown_units": max_drawdown,
        "fees_units": fees,
        "gross_profit_units": gross_profit,
        "gross_loss_units": gross_loss,
        "profit_factor": round(gross_profit / gross_loss, 3) if gross_loss else ("inf" if gross_profit else 0.0),
        "position_units": position,
    }


def prediction_metrics(rows: list[dict[str, Any]], predictions: list[list[float]], threshold: float) -> tuple[dict[str, Any], list[int]]:
    actions: list[int] = []
    correct = 0
    for row, prediction in zip(rows, predictions):
        direction = max((1, 2), key=lambda index: prediction[index])
        action = direction if prediction[direction] >= threshold else 0
        actions.append(1 if action == 1 else -1 if action == 2 else 0)
        if action == int(row["label"]):
            correct += 1
    return {
        "accuracy": round(correct / len(rows), 6) if rows else 0.0,
        "label_counts": dict(Counter(CLASS_NAMES[int(row["label"])] for row in rows)),
        "action_counts": dict(Counter(CLASS_NAMES[0] if action == 0 else CLASS_NAMES[action] for action in actions)),
        "log_loss": round(log_loss(rows, predictions), 6),
        "brier": round(brier_score(rows, predictions), 6),
    }, actions


def choose_temperature(rows: list[dict[str, Any]], weights: list[list[float]], means: list[float], scales: list[float]) -> tuple[float, float]:
    candidates = (0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0)
    scored = [(log_loss(rows, [probabilities(row, weights, means, scales, temperature) for row in rows]), temperature) for temperature in candidates]
    loss, temperature = min(scored)
    return temperature, loss


def generic_laya_reference(path: Path) -> dict[str, Any]:
    report = json.loads(path.read_text(encoding="utf-8"))
    result: dict[str, Any] = {}
    for row in report["runs"]:
        if row.get("model") == "laya" and row.get("cost_scenario") == "base":
            tape = row.get("decision_tape", {})
            result[row["split"]] = {
                "return_ppm": row["return_ppm"],
                "profit_factor": row["profit_factor"],
                "fills": row["fills"],
                "decisions": tape.get("decisions", 0),
                "gated_holds": tape.get("gated_holds", 0),
                "proposed_actions": tape.get("proposed_actions", {}),
            }
    return result


def make_markdown(report: dict[str, Any]) -> str:
    rows = report["evaluation"]
    table = [
        "| Split | Cost | BTC model return | PF | Max DD | Fills | Laya return | Laya fills |",
        "|---|---|---:|---:|---:|---:|---:|---:|",
    ]
    for row in rows:
        laya = report["generic_laya_reference"].get(row["split"], {})
        table.append(
            f"| {row['split']} | {row['cost_scenario']} | {row['return_ppm'] / 10_000:.4f}% | "
            f"{row['profit_factor']} | {row['max_drawdown_units']} | {row['fills']} | "
            f"{laya.get('return_ppm', 0) / 10_000:.4f}% | {laya.get('fills', 0)} |"
        )
    holdout = next(row for row in rows if row["split"] == "holdout_2026" and row["cost_scenario"] == "base")
    approved = holdout["return_ppm"] > 0 and isinstance(holdout["profit_factor"], (int, float)) and holdout["profit_factor"] > 1.15 and holdout["fills"] >= 20
    decision = "eligible for a bounded paper shadow run" if approved else "reject for paper promotion; continue research"
    lines = [
        "# BTC-specific model OOS validation",
        "",
        "This report trains a small BTC-specific softmax action model from the normalized one-minute replay stream.",
        "It is a research calibration artifact, not a replacement for the C++ execution path.",
        "The generic Laya checkpoint remains frozen; its existing C++ replay is included as the reference model.",
        "",
        "## Results",
        "",
        *table,
        "",
        f"Validation-selected temperature: `{report['calibration']['temperature']}`; action threshold: `{report['calibration']['threshold']}`.",
        f"2026 holdout decision: **{decision}**.",
        "",
        "## Protocol",
        "",
        f"- Input: `{report['input']['path']}`; `{report['input']['rows']:,}` one-minute events; SHA-256 `{report['input']['sha256']}`.",
        f"- Feature cadence: every `{report['protocol']['stride']}` minutes; label horizon: `{report['protocol']['horizon']}` minutes.",
        "- Features use only current and past data: causal returns, rolling volatility, book flow imbalance, spread, and UTC hour seasonality.",
        "- Labels use future executable bid/ask: long/short only when the best direction clears round-trip base fee/slippage plus a 2 bp margin.",
        "- Train: 2025-01-01 through 2025-06-30. Temperature and action threshold: 2025-07-01 through 2025-12-31.",
        "- Holdout: 2026-01-01 through 2026-08-31; no model or threshold choice uses it.",
        "- Simulation uses integer price/quantity units, next-sample execution, taker fees, and adverse slippage.",
        "",
        "## Interpretation",
        "",
        "A positive prediction score is not sufficient for promotion. The holdout must also clear the PF, drawdown, and trade-count gates in the JSON artifact.",
        "The generic Laya comparison has a different production replay cadence (every 1,000 normalized events) and is therefore a reference, not a paired significance test.",
        "This run does not fine-tune Laya's neural checkpoint; it calibrates a separate BTC-specific model against the same market data.",
        "",
        "## Limitations",
        "",
        "- The book is a synthetic two-sided book around minute closes, not historical Binance full-L2.",
        "- The source cache ends on 2026-08-31; September is not inferred.",
        "- A linear classifier and one validation window cannot establish durable alpha. A later paper shadow run needs rolling retraining and uncertainty intervals.",
        "- No live or testnet order submission is enabled.",
        "",
        "Generated by `scripts/run_btc_model_oos_report.py`.",
        "",
    ]
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=Path("data/replay/btc-2025-2026.csv"))
    parser.add_argument("--replay-report", type=Path, default=Path("reports/btc_cpp_replay_validation/results.json"))
    parser.add_argument("--output-dir", type=Path, default=Path("reports/btc_model_oos_validation"))
    parser.add_argument("--stride", type=int, default=60, help="sample cadence in one-minute rows")
    parser.add_argument("--horizon", type=int, default=60, help="forward label horizon in one-minute rows")
    parser.add_argument("--label-margin", type=float, default=0.0002)
    args = parser.parse_args()
    if not args.input.exists():
        raise SystemExit(f"missing replay input: {args.input}")
    if not args.replay_report.exists():
        raise SystemExit(f"missing generic Laya replay report: {args.replay_report}")
    if args.stride <= 0 or args.horizon <= 0:
        raise SystemExit("stride and horizon must be positive")

    timestamps, bids, asks, bid_qty, ask_qty, mids = load_csv(args.input)
    samples = build_samples(timestamps, bids, asks, bid_qty, ask_qty, mids, args.stride, args.horizon, args.label_margin)
    grouped = {split: [row for row in samples if row["split"] == split] for split in SPLITS}
    if not grouped["development_train"] or not grouped["development_validation"] or not grouped["holdout_2026"]:
        raise SystemExit(f"insufficient split samples: { {name: len(rows) for name, rows in grouped.items()} }")
    means, scales = standardizer(grouped["development_train"])
    weights = train_softmax(grouped["development_train"], means, scales)
    temperature, validation_loss = choose_temperature(grouped["development_validation"], weights, means, scales)
    validation_predictions = [probabilities(row, weights, means, scales, temperature) for row in grouped["development_validation"]]

    threshold_scores: list[tuple[int, float, int]] = []
    for threshold in (0.40, 0.45, 0.50, 0.55, 0.60, 0.65, 0.70):
        _, actions = prediction_metrics(grouped["development_validation"], validation_predictions, threshold)
        metrics = simulate(grouped["development_validation"], actions, bids, asks, mids, *COSTS["base"])
        threshold_scores.append((metrics["return_ppm"], threshold, metrics["fills"]))
    _, threshold, _ = max(threshold_scores, key=lambda item: (item[0], item[2], item[1]))

    evaluation: list[dict[str, Any]] = []
    prediction_summary: dict[str, Any] = {}
    for split, rows in grouped.items():
        predictions = [probabilities(row, weights, means, scales, temperature) for row in rows]
        summary, actions = prediction_metrics(rows, predictions, threshold)
        prediction_summary[split] = summary
        for cost_scenario, costs in COSTS.items():
            metrics = simulate(rows, actions, bids, asks, mids, *costs)
            evaluation.append({"split": split, "cost_scenario": cost_scenario, **metrics})

    report = {
        "schema": "astra.report.btc-model-oos.v1",
        "generated_at": datetime.now(UTC).isoformat(),
        "input": {"path": str(args.input), "sha256": sha256(args.input), "rows": len(timestamps), "start_ns": int(timestamps[0]), "end_ns": int(timestamps[-1])},
        "protocol": {"stride": args.stride, "horizon": args.horizon, "label_margin": args.label_margin, "splits": {name: len(rows) for name, rows in grouped.items()}},
        "features": list(FEATURE_NAMES),
        "model": {"type": "standard-library linear softmax", "classes": list(CLASS_NAMES), "train_label_counts": dict(Counter(CLASS_NAMES[int(row["label"])] for row in grouped["development_train"]))},
        "artifact": {"feature_means": means, "feature_scales": scales, "weights": weights},
        "calibration": {"temperature": temperature, "validation_log_loss": round(validation_loss, 6), "threshold": threshold, "threshold_candidates": [{"return_ppm": value, "threshold": candidate, "fills": fills} for value, candidate, fills in threshold_scores]},
        "prediction_summary": prediction_summary,
        "evaluation": evaluation,
        "generic_laya_reference": generic_laya_reference(args.replay_report),
        "decision": {"paper_approved": False, "reason": "This is the first BTC-specific OOS experiment; promotion remains manual and the holdout gates are reported above."},
    }
    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    (args.output_dir / "README.md").write_text(make_markdown(report), encoding="utf-8")
    print(json.dumps({"output": str(args.output_dir), "samples": report["protocol"]["splits"], "holdout": next(row for row in evaluation if row["split"] == "holdout_2026" and row["cost_scenario"] == "base")}, indent=2))


if __name__ == "__main__":
    main()
