#!/usr/bin/env python3
"""Train separate BTC direction experts and route them by causal market regime."""

from __future__ import annotations

import argparse
import json
import sys
import tempfile
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))
import run_btc_regime_router_oos as router
import run_btc_tick_baseline_oos as baseline

EXPERTS = ("trend_up", "trend_down", "range")


def expert_name(row: dict[str, Any]) -> str:
    name = str(row["regime"])
    if name.startswith("trend_up"):
        return "trend_up"
    if name.startswith("trend_down"):
        return "trend_down"
    return "range"


def train_experts(train_rows: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    experts = {}
    for name in EXPERTS:
        rows = [row for row in train_rows if expert_name(row) == name]
        means, scales = baseline.standardizer(rows)
        experts[name] = {
            "means": means,
            "scales": scales,
            "weights": baseline.train(rows, means, scales),
            "train_rows": len(rows),
        }
    return experts


def predict_routed(
    rows: list[dict[str, Any]], experts: dict[str, dict[str, Any]]
) -> list[list[float]]:
    predictions = []
    for row in rows:
        expert = experts[expert_name(row)]
        predictions.append(
            baseline.predict(
                [row], expert["weights"], expert["means"], expert["scales"]
            )[0]
        )
    return predictions


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
        "--output-dir", type=Path, default=Path("reports/btc_regime_experts_oos")
    )
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    prices = baseline.load_replay_prices(args.input)
    rows = baseline.load_samples(
        args.samples, baseline.load_replay_sequences(args.input)
    )
    train_rows = [row for row in rows if row["split"] == "development_train"]
    router.add_efficiency(rows, prices)
    thresholds = {
        "efficiency": router.quantile(
            [float(row["efficiency_60"]) for row in train_rows], 0.60
        ),
        "high_vol": router.quantile(
            [float(row["features"][4]) for row in train_rows], 0.75
        ),
        "low_vol": router.quantile(
            [float(row["features"][4]) for row in train_rows], 0.25
        ),
    }
    router.assign_regimes(rows, thresholds)
    grouped = {
        split: [row for row in rows if row["split"] == split]
        for split in baseline.SPLITS
    }
    experts = train_experts(train_rows)
    predictions = {
        split: predict_routed(grouped[split], experts) for split in baseline.SPLITS
    }
    periods = {
        "development_train": (1735689600000000000, 1751327999000000000),
        "development_validation": (1751328000000000000, 1767225599000000000),
        "holdout_2026": (1767225600000000000, 1788220799000000000),
    }
    candidates = []
    with tempfile.TemporaryDirectory(prefix="astra-regime-experts-") as directory:
        scratch = Path(directory)
        for candidate_index, threshold in enumerate(
            (0.34, 0.36, 0.38, 0.40, 0.42, 0.45, 0.50, 0.55, 0.60)
        ):
            tape = scratch / f"validation-{candidate_index}.csv"
            output = scratch / f"validation-{candidate_index}.json"
            baseline.write_tape(
                tape,
                grouped["development_validation"],
                predictions["development_validation"],
                threshold,
            )
            execution = baseline.run_cpp(
                args.binary,
                args.input,
                tape,
                output,
                *periods["development_validation"],
                *baseline.COSTS["base"],
            )
            prediction = baseline.prediction_report(
                grouped["development_validation"],
                predictions["development_validation"],
                threshold,
            )[0]
            candidates.append(
                {
                    "threshold": threshold,
                    "validation_return_ppm": execution["return_ppm"],
                    "validation_profit_factor": execution["profit_factor"],
                    "validation_fills": execution["fills"],
                    "proxy_score": prediction["proxy_score"],
                }
            )
    eligible = [
        candidate for candidate in candidates if candidate["validation_fills"] >= 30
    ]
    selected = max(
        eligible,
        key=lambda candidate: (
            candidate["validation_return_ppm"],
            candidate["validation_profit_factor"],
            -candidate["validation_fills"],
        ),
    )
    threshold = float(selected["threshold"])
    tapes = {}
    cpp = {}
    for split in baseline.SPLITS:
        tape = args.output_dir / f"{split}-experts-tape.csv"
        baseline.write_tape(tape, grouped[split], predictions[split], threshold)
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
    for period_name, period in holdout_periods.items():
        period_results[period_name] = {}
        for cost_name, (fee, slippage) in baseline.COSTS.items():
            output = args.output_dir / f"{period_name}-cpp-{cost_name}.json"
            period_results[period_name][cost_name] = baseline.run_cpp(
                args.binary,
                args.input,
                args.output_dir / "holdout_2026-experts-tape.csv",
                output,
                *period,
                fee,
                slippage,
            )
    report = {
        "schema": "astra.report.btc-regime-experts-oos.v1",
        "generated_at": datetime.now(UTC).isoformat(),
        "input": {
            "replay": str(args.input),
            "replay_sha256": baseline.sha256(args.input),
            "samples": str(args.samples),
            "samples_sha256": baseline.sha256(args.samples),
        },
        "protocol": {
            "router": "causal trend_up/trend_down/range",
            "regime_thresholds": thresholds,
            "experts": {
                name: {"train_rows": expert["train_rows"]}
                for name, expert in experts.items()
            },
            "sample_counts": {split: len(grouped[split]) for split in baseline.SPLITS},
        },
        "calibration": {
            "objective": "maximum C++ base-cost validation return with at least 30 fills",
            "selected": selected,
            "candidates": candidates,
        },
        "prediction": {
            split: baseline.prediction_report(
                grouped[split], predictions[split], threshold
            )[0]
            for split in baseline.SPLITS
        },
        "decision_tapes": tapes,
        "cpp_tape_evaluation": cpp,
        "holdout_periods": period_results,
        "decision": {
            "paper_approved": False,
            "reason": "Regime experts must clear aggregate, stress-cost, and subperiod gates before paper use.",
        },
    }
    (args.output_dir / "results.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8"
    )
    print(
        json.dumps(
            {
                "selected": selected,
                "experts": report["protocol"]["experts"],
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
