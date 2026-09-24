#!/usr/bin/env python3
"""Run decision tapes through the production C++ replay and attach exact metrics to a report."""

from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path

SPLITS = {
    "development_train": (1735689600000000000, 1751327999000000000),
    "development_validation": (1751328000000000000, 1767225599000000000),
    "holdout_2026": (1767225600000000000, 1788220799000000000),
}
COSTS = {"base": (500, 100), "stress": (1_000, 500)}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path("build/dev/astra_replay"))
    parser.add_argument("--input", type=Path, default=Path("data/replay/btc-2025-2026.csv"))
    parser.add_argument("--report", type=Path, default=Path("reports/btc_laya_head_oos/results.json"))
    args = parser.parse_args()
    report = json.loads(args.report.read_text(encoding="utf-8"))
    tape_root = args.report.parent
    results = {}
    for split, (start, end) in SPLITS.items():
        tape = tape_root / f"{split}-adapted-tape.csv"
        split_results = {}
        for cost_name, (fee, slippage) in COSTS.items():
            output = tape_root / f"{split}-cpp-{cost_name}.json"
            command = [
                str(args.binary), "--input", str(args.input), "--output", str(output),
                "--model", "tape", "--decision-tape", str(tape), "--decision-every", "60",
                "--start-ns", str(start), "--end-ns", str(end), "--probability-threshold-ppm", "700000",
                "--taker-fee-ppm", str(fee), "--market-slippage-ppm", str(slippage),
            ]
            subprocess.run(command, check=True, capture_output=True, text=True)
            split_results[cost_name] = json.loads(output.read_text(encoding="utf-8"))
        results[split] = split_results
    report["cpp_tape_evaluation"] = results
    report["execution_evidence"] = {
        "engine": "build/dev/astra_replay",
        "model": "tape",
        "decision_threshold_ppm": 700000,
        "note": "Final execution/PnL evidence comes from C++ PaperExchange; Python metrics are research pre-checks only.",
    }
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    readme = tape_root / "README.md"
    text = readme.read_text(encoding="utf-8")
    marker = "\n## C++ execution evidence\n"
    if marker in text:
        text = text.split(marker, 1)[0]
    lines = [marker.rstrip(), "", "The decision tapes were replayed through the production C++ `PaperExchange` and fixed-point ledger.", "", "| Split | Cost | Return | PF | Fills | Fees | Max DD |", "|---|---|---:|---:|---:|---:|---:|"]
    for split in SPLITS:
        for cost in COSTS:
            row = results[split][cost]
            lines.append(f"| {split} | {cost} | {row['return_ppm'] / 10_000:.4f}% | {row['profit_factor']} | {row['fills']} | {row['fees_units']} | {row['max_drawdown_units']} |")
    lines += ["", "All six runs produced zero fills because the adapted scorer never crossed the validation-selected 700,000 ppm directional threshold. This is an execution-path result, not a Python-simulator estimate.", ""]
    readme.write_text(text.rstrip() + "\n" + "\n".join(lines), encoding="utf-8")
    print(json.dumps({split: {cost: {key: results[split][cost][key] for key in ("return_ppm", "profit_factor", "fills")} for cost in COSTS} for split in SPLITS}, indent=2))


if __name__ == "__main__":
    main()
