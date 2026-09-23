#!/usr/bin/env python3
"""Run the C++ replay against baseline, hold, and optionally Laya.

The executable uses the production FeatureBuilder, PaperExchange, RiskEngine, and (for Laya)
the production HTTP gateway. The report stores only derived metrics and small decision/fill
tapes; the normalized input stream remains under ignored ``data/``.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

SPLITS = {
    "development_train": ("2025-01-01T00:00:00+00:00", "2025-06-30T23:59:59+00:00"),
    "development_validation": ("2025-07-01T00:00:00+00:00", "2025-12-31T23:59:59+00:00"),
    "holdout_2026": ("2026-01-01T00:00:00+00:00", "2026-08-31T23:59:59+00:00"),
}


def ns(value: str) -> int:
    return int(datetime.fromisoformat(value).timestamp() * 1_000_000_000)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run_one(
    binary: Path,
    input_path: Path,
    output_dir: Path,
    split: str,
    model: str,
    decision_every: int,
    include_detail: bool,
) -> dict[str, Any]:
    raw_dir = output_dir / "raw"
    raw_dir.mkdir(parents=True, exist_ok=True)
    metrics_path = raw_dir / f"{split}-{model}.json"
    detail_path = raw_dir / f"{split}-{model}.jsonl"
    start, end = SPLITS[split]
    command = [
        str(binary),
        "--input",
        str(input_path),
        "--output",
        str(metrics_path),
        "--model",
        model,
        "--decision-every",
        str(decision_every),
        "--start-ns",
        str(ns(start)),
        "--end-ns",
        str(ns(end)),
    ]
    if include_detail:
        command.extend(("--detail", str(detail_path)))
    completed = subprocess.run(command, check=True, capture_output=True, text=True)
    result = json.loads(metrics_path.read_text(encoding="utf-8"))
    result["split"] = split
    result["period"] = {"start": start, "end": end}
    result["metrics_sha256"] = sha256(metrics_path)
    if include_detail and detail_path.exists():
        decisions = []
        errors = 0
        latencies = []
        proposed_actions: dict[str, int] = {}
        gated_holds = 0
        for line in detail_path.read_text(encoding="utf-8").splitlines():
            event = json.loads(line)
            if event.get("event") != "decision":
                continue
            decisions.append(event)
            latencies.append(int(event.get("latency_ns", 0)) / 1_000_000)
            proposed = str(event.get("proposed_action", event.get("action", "unknown")))
            proposed_actions[proposed] = proposed_actions.get(proposed, 0) + 1
            if event.get("gate_hold", False):
                gated_holds += 1
            if not event.get("succeeded", False):
                errors += 1
        latencies.sort()
        result["decision_tape"] = {
            "path": str(detail_path),
            "sha256": sha256(detail_path),
            "decisions": len(decisions),
            "errors": errors,
            "proposed_actions": proposed_actions,
            "gated_holds": gated_holds,
            "latency_ms": {
                "mean": round(sum(latencies) / len(latencies), 3) if latencies else 0.0,
                "p50": round(latencies[len(latencies) // 2], 3) if latencies else 0.0,
                "p95": round(latencies[min(len(latencies) - 1, int(len(latencies) * 0.95))], 3)
                if latencies
                else 0.0,
            },
        }
    result["stdout_tail"] = completed.stdout.strip().splitlines()[-1:]
    return result


def markdown(report: dict[str, Any]) -> str:
    rows = [
        "| Split | Model | Return | PF | Max DD | Fills | Orders | Fees |",
        "|---|---|---:|---:|---:|---:|---:|---:|",
    ]
    for result in report["runs"]:
        return_pct = int(result["return_ppm"]) / 10_000
        rows.append(
            f"| {result['split']} | {result['model']} | {return_pct:.4f}% | "
            f"{result['profit_factor']} | {result['max_drawdown_units']} | "
            f"{result['fills']} | {result['accepted_orders']} | {result['fees_units']} |"
        )
    laya = [row for row in report["runs"] if row["model"] == "laya"]
    laya_errors = sum(row.get("decision_tape", {}).get("errors", 0) for row in laya)
    laya_decisions = sum(row.get("decision_tape", {}).get("decisions", 0) for row in laya)
    laya_gated = sum(row.get("decision_tape", {}).get("gated_holds", 0) for row in laya)
    lines = [
        "# BTC C++ replay validation",
        "",
        "This report sends the same normalized event stream through the C++ `FeatureBuilder`,",
        "`PaperExchange`, risk gate, accounting, and model decision boundary used by paper mode.",
        "The input is a one-minute aggregate stream with a synthetic two-sided book around the",
        "minute close; it validates causal state transitions and costs, not full-L2 queue execution.",
        "",
        "## Results",
        "",
        *rows,
        "",
        (
            f"Laya decision calls: `{laya_decisions}`; gateway errors: `{laya_errors}`; "
            f"directional proposals gated to hold: `{laya_gated}`."
        ),
        "The model gate requires 550,000 ppm directional probability. `hold` is a control that",
        "never submits an order. The flow baseline is intentionally simple and is not an alpha claim.",
        "",
        "## Protocol",
        "",
        f"- Input: `{report['input']['path']}`; `{report['input']['minutes']:,}` events.",
        f"- Data range: `{report['input']['start']}` through `{report['input']['last']}`.",
        "- Development train: 2025-01-01 through 2025-06-30.",
        "- Development validation: 2025-07-01 through 2025-12-31.",
        "- Untouched holdout: 2026-01-01 through 2026-08-31, matching the available cache cutoff.",
        "- Decision cadence: every 1,000 normalized events; an order is submitted on the next event.",
        "- Simulated execution: taker fee 5 bps, market slippage 1 bp, one-minute synthetic spread.",
        "- Initial cash: 100,000 USDT-equivalent account units; target position: +/-0.1 BTC.",
        "",
        "## Decision",
        "",
        "This is a validation of the production replay plumbing, not an approval of a strategy.",
        "The flow baseline must be rejected because it loses after costs in the aggregate period;",
        "Laya must also remain rejected when its holdout net return, PF, or trade count is inadequate.",
        "No live or testnet execution is enabled by this report.",
        "",
        "## Limitations",
        "",
        "- The source cache ends on 2026-08-31; September data is not silently extrapolated.",
        "- The synthetic book is not a Binance L2 reconstruction and cannot prove queue placement or maker fills.",
        "- Laya's generic checkpoint is uncalibrated for trading; probabilities are recorded for audit only.",
        "- A profitable replay would still require a forward paper acceptance run before any future scope change.",
        "",
        "Generated by `scripts/run_btc_cpp_replay_report.py`.",
        "",
    ]
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path("build/dev/astra_replay"))
    parser.add_argument("--input", type=Path, default=Path("data/replay/btc-2025-2026.csv"))
    parser.add_argument("--output-dir", type=Path, default=Path("reports/btc_cpp_replay_validation"))
    parser.add_argument("--decision-every", type=int, default=1_000)
    parser.add_argument("--models", default="hold,baseline,laya")
    parser.add_argument("--skip-details", action="store_true")
    args = parser.parse_args()
    if not args.binary.exists():
        raise SystemExit(f"replay binary does not exist: {args.binary}")
    if not args.input.exists():
        raise SystemExit(f"replay input does not exist: {args.input}")
    manifest_path = args.input.with_suffix(".manifest.json")
    if not manifest_path.exists():
        raise SystemExit(f"replay input manifest does not exist: {manifest_path}")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    models = [model.strip() for model in args.models.split(",") if model.strip()]
    unknown = set(models) - {"hold", "baseline", "laya"}
    if unknown:
        raise SystemExit(f"unknown models: {sorted(unknown)}")
    runs = []
    for split in SPLITS:
        for model in models:
            print(f"running {split}/{model}", flush=True)
            runs.append(
                run_one(
                    args.binary,
                    args.input,
                    args.output_dir,
                    split,
                    model,
                    args.decision_every,
                    not args.skip_details,
                )
            )
    report = {
        "schema": "astra.report.btc-cpp-replay.v1",
        "generated_at": datetime.now(UTC).isoformat(),
        "input": manifest | {"path": str(args.input), "sha256": sha256(args.input)},
        "protocol": {
            "decision_every": args.decision_every,
            "models": models,
            "feed_latency_ns": 1_000_000,
            "taker_fee_ppm": 5_000,
            "market_slippage_ppm": 1_000,
            "next_event_execution": True,
            "paper_only": True,
        },
        "runs": runs,
        "decision": {
            "strategy_approved": False,
            "paper_approved": False,
            "live_approved": False,
            "reason": "This replay validates the execution path; current aggregate baseline and uncalibrated Laya are not strategy approvals.",
        },
    }
    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    (args.output_dir / "README.md").write_text(markdown(report), encoding="utf-8")
    print(json.dumps(report["decision"], sort_keys=True))


if __name__ == "__main__":
    main()
