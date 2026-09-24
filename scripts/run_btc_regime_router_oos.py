#!/usr/bin/env python3
"""Evaluate a causal market-regime gate around the BTC softmax baseline."""

from __future__ import annotations

import argparse
import json
import math
import sys
import tempfile
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))
import run_btc_tick_baseline_oos as baseline


def quantile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, round((len(ordered) - 1) * fraction)))
    return ordered[index]


def regime(row: dict[str, Any], thresholds: dict[str, float]) -> str:
    ret_60 = float(row["features"][3])
    volatility = float(row["features"][4])
    efficiency = float(row.get("efficiency_60", 0.0))
    trend = (
        "trend_up"
        if efficiency >= thresholds["efficiency"] and ret_60 >= 0
        else "trend_down"
        if efficiency >= thresholds["efficiency"]
        else "range"
    )
    vol = (
        "high_vol"
        if volatility >= thresholds["high_vol"]
        else "low_vol"
        if volatility <= thresholds["low_vol"]
        else "mid_vol"
    )
    return f"{trend}_{vol}"


def add_efficiency(rows: list[dict[str, Any]], prices: list[float]) -> None:
    for row in rows:
        index = int(row["sequence"]) - 1
        if index < 60:
            efficiency = 0.0
        else:
            numerator = abs(float(row["features"][3]))
            denominator = sum(
                abs(math.log(prices[offset] / prices[offset - 1]))
                for offset in range(index - 59, index + 1)
                if prices[offset - 1] > 0
            )
            efficiency = numerator / denominator if denominator else 0.0
        row["efficiency_60"] = efficiency


def assign_regimes(rows: list[dict[str, Any]], thresholds: dict[str, float]) -> None:
    for row in rows:
        row["regime"] = regime(row, thresholds)


def gated_probabilities(
    rows: list[dict[str, Any]],
    probabilities: list[list[float]],
    allowed: set[str],
    threshold: float,
) -> tuple[dict[str, Any], list[int]]:
    gated = []
    for row, probability in zip(rows, probabilities):
        gated.append(probability if row["regime"] in allowed else [1.0, 0.0, 0.0])
    return baseline.prediction_report(rows, gated, threshold)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--samples",
        type=Path,
        default=Path(
            "/home/ldtdev/qt/mmtick/data/order_flow_cache/btc-laya-1m-samples-v1.csv.gz"
        ),
    )
    parser.add_argument(
        "--input", type=Path, default=Path("data/replay/btc-2025-2026.csv")
    )
    parser.add_argument("--binary", type=Path, default=Path("build/dev/astra_replay"))
    parser.add_argument(
        "--output-dir", type=Path, default=Path("reports/btc_regime_router_oos")
    )
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    replay_sequences = baseline.load_replay_sequences(args.input)
    prices = baseline.load_replay_prices(args.input)
    rows = baseline.load_samples(args.samples, replay_sequences)
    train_rows = [row for row in rows if row["split"] == "development_train"]
    add_efficiency(rows, prices)
    thresholds = {
        "efficiency": quantile(
            [float(row["efficiency_60"]) for row in train_rows], 0.60
        ),
        "high_vol": quantile([float(row["features"][4]) for row in train_rows], 0.75),
        "low_vol": quantile([float(row["features"][4]) for row in train_rows], 0.25),
    }
    assign_regimes(rows, thresholds)
    grouped = {
        split: [row for row in rows if row["split"] == split]
        for split in baseline.SPLITS
    }
    means, scales = baseline.standardizer(train_rows)
    weights = baseline.train(train_rows, means, scales)
    probabilities = {
        split: baseline.predict(grouped[split], weights, means, scales)
        for split in baseline.SPLITS
    }
    regimes = sorted({str(row["regime"]) for row in rows})
    candidates: list[dict[str, Any]] = [{"name": "all", "allowed": regimes}]
    candidates.extend({"name": name, "allowed": [name]} for name in regimes)
    candidates.extend(
        (
            {
                "name": "trend",
                "allowed": [name for name in regimes if name.startswith("trend_")],
            },
            {
                "name": "range",
                "allowed": [name for name in regimes if name.startswith("range_")],
            },
            {
                "name": "high_vol",
                "allowed": [name for name in regimes if name.endswith("high_vol")],
            },
            {
                "name": "low_vol",
                "allowed": [name for name in regimes if name.endswith("low_vol")],
            },
        )
    )
    threshold_candidates = (0.34, 0.36, 0.38, 0.40, 0.42, 0.45, 0.50)
    scored = []
    validation_period = (1751328000000000000, 1767225599000000000)
    with tempfile.TemporaryDirectory(prefix="astra-regime-calibration-") as directory:
        scratch = Path(directory)
        for candidate_index, candidate in enumerate(candidates):
            allowed = set(candidate["allowed"])
            gated = [
                probability if row["regime"] in allowed else [1.0, 0.0, 0.0]
                for row, probability in zip(
                    grouped["development_validation"],
                    probabilities["development_validation"],
                )
            ]
            for threshold_index, probability_threshold in enumerate(
                threshold_candidates
            ):
                prediction, _ = baseline.prediction_report(
                    grouped["development_validation"], gated, probability_threshold
                )
                tape = scratch / f"candidate-{candidate_index}-{threshold_index}.csv"
                output = scratch / f"candidate-{candidate_index}-{threshold_index}.json"
                baseline.write_tape(
                    tape,
                    grouped["development_validation"],
                    gated,
                    probability_threshold,
                )
                execution = baseline.run_cpp(
                    args.binary,
                    args.input,
                    tape,
                    output,
                    *validation_period,
                    *baseline.COSTS["base"],
                )
                scored.append(
                    {
                        **candidate,
                        "threshold": probability_threshold,
                        "proxy_score": prediction["proxy_score"],
                        "directional_actions": prediction["directional_actions"],
                        "validation_return_ppm": execution["return_ppm"],
                        "validation_profit_factor": execution["profit_factor"],
                        "validation_fills": execution["fills"],
                    }
                )
    eligible = [item for item in scored if item["validation_fills"] >= 30]
    selected = max(
        eligible,
        key=lambda item: (
            item["validation_return_ppm"],
            item["validation_profit_factor"],
            -item["validation_fills"],
        ),
    )
    selected_allowed = set(selected["allowed"])
    tapes = {}
    cpp = {}
    periods = {
        "development_train": (1735689600000000000, 1751327999000000000),
        "development_validation": (1751328000000000000, 1767225599000000000),
        "holdout_2026": (1767225600000000000, 1788220799000000000),
    }
    for split in baseline.SPLITS:
        gated = [
            probability if row["regime"] in selected_allowed else [1.0, 0.0, 0.0]
            for row, probability in zip(grouped[split], probabilities[split])
        ]
        tape = args.output_dir / f"{split}-regime-tape.csv"
        baseline.write_tape(tape, grouped[split], gated, float(selected["threshold"]))
        tapes[split] = {
            "path": str(tape),
            "sha256": baseline.sha256(tape),
            "bytes": tape.stat().st_size,
        }
        cpp[split] = {}
        for cost_name, (fee, slippage) in baseline.COSTS.items():
            output = args.output_dir / f"{split}-cpp-{cost_name}.json"
            cpp[split][cost_name] = baseline.run_cpp(
                args.binary, args.input, tape, output, *periods[split], fee, slippage
            )
    holdout_periods = {
        "2026_q1": (1767225600000000000, 1775001599000000000),
        "2026_q2": (1775001600000000000, 1782863999000000000),
        "2026_jul_aug": (1782864000000000000, 1788220799000000000),
    }
    period_results = {}
    holdout_tape = args.output_dir / "holdout_2026-regime-tape.csv"
    for period_name, period in holdout_periods.items():
        period_results[period_name] = {}
        for cost_name, (fee, slippage) in baseline.COSTS.items():
            output = args.output_dir / f"{period_name}-cpp-{cost_name}.json"
            period_results[period_name][cost_name] = baseline.run_cpp(
                args.binary, args.input, holdout_tape, output, *period, fee, slippage
            )
    report = {
        "schema": "astra.report.btc-regime-router-oos.v1",
        "generated_at": datetime.now(UTC).isoformat(),
        "input": {
            "replay": str(args.input),
            "replay_sha256": baseline.sha256(args.input),
            "samples": str(args.samples),
            "samples_sha256": baseline.sha256(args.samples),
        },
        "protocol": {
            "regime_thresholds": thresholds,
            "regimes": regimes,
            "train_only_thresholds": True,
            "direction_model": "class-weighted softmax baseline",
            "sample_counts": {split: len(grouped[split]) for split in baseline.SPLITS},
        },
        "calibration": {
            "objective": "maximum C++ base-cost validation return with at least 30 fills",
            "selected": selected,
            "candidates": scored,
        },
        "prediction": {
            split: gated_probabilities(
                grouped[split],
                probabilities[split],
                selected_allowed,
                float(selected["threshold"]),
            )[0]
            for split in baseline.SPLITS
        },
        "decision_tapes": tapes,
        "cpp_tape_evaluation": cpp,
        "holdout_periods": period_results,
        "decision": {
            "paper_approved": False,
            "reason": "Regime router is a research experiment and must pass rolling OOS and cost gates.",
        },
    }
    (args.output_dir / "results.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8"
    )
    print(
        json.dumps(
            {
                "selected": selected,
                "holdout_base": {
                    key: cpp["holdout_2026"]["base"][key]
                    for key in ("return_ppm", "profit_factor", "fills")
                },
                "holdout_stress": {
                    key: cpp["holdout_2026"]["stress"][key]
                    for key in ("return_ppm", "profit_factor", "fills")
                },
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
