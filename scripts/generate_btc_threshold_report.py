#!/usr/bin/env python3
"""Build the Astra BTC threshold-validation report from immutable research artifacts.

The large Binance archives stay in mmtick. This report intentionally stores only a
small manifest and the derived research result, so cloning Astra does not duplicate
the historical data set.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from collections import Counter
from datetime import UTC, datetime
from pathlib import Path
from statistics import mean, quantiles
from typing import Any


DEFAULT_SOURCE = Path(
    "/home/ldtdev/qt/mmtick/reports/experiments/"
    "btc_tick_profittaker/2026-09-14-expanded/results.json"
)
DEFAULT_AUDIT = Path("data/paper-events.jsonl")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_audit(path: Path) -> dict[str, Any]:
    decisions: list[dict[str, Any]] = []
    if not path.exists():
        return {"available": False, "path": str(path), "events": 0}
    with path.open(encoding="utf-8") as handle:
        for line in handle:
            try:
                event = json.loads(line)
            except json.JSONDecodeError:
                continue
            if event.get("event") == "decision":
                decisions.append(event)
    if not decisions:
        return {
            "available": True,
            "path": str(path),
            "sha256": sha256(path),
            "events": 0,
            "orders": 0,
            "fills": 0,
        }
    latencies_ms = [float(row.get("latency_ns", 0)) / 1_000_000 for row in decisions]
    percentile = quantiles(latencies_ms, n=100, method="inclusive") if len(latencies_ms) > 1 else [latencies_ms[0]]
    return {
        "available": True,
        "path": str(path),
        "sha256": sha256(path),
        "events": len(decisions),
        "first_sequence": decisions[0].get("sequence"),
        "last_sequence": decisions[-1].get("sequence"),
        "actions": dict(Counter(row.get("action", "unknown") for row in decisions)),
        "orders": 0,
        "fills": 0,
        "latency_ms": {
            "mean": round(mean(latencies_ms), 3),
            "p50": round(percentile[49] if len(percentile) >= 50 else percentile[0], 3),
            "p95": round(percentile[94] if len(percentile) >= 95 else percentile[-1], 3),
            "p99": round(percentile[98] if len(percentile) >= 99 else percentile[-1], 3),
        },
    }


def build(source_path: Path, audit_path: Path) -> dict[str, Any]:
    source = json.loads(source_path.read_text(encoding="utf-8"))
    audit = load_audit(audit_path)
    selected = source.get("selected")
    return {
        "schema": "astra.report.btc-threshold-validation.v1",
        "generated_at": datetime.now(UTC).isoformat(),
        "scope": {
            "instrument": "BTCUSDT",
            "venue": "Binance USD-M Futures public aggregate trades",
            "purpose": "cost-aware threshold validation before paper exposure",
            "paper_only": True,
        },
        "reference_replay": {
            "source_report": str(source_path),
            "source_sha256": sha256(source_path),
            "strategy": source.get("strategy"),
            "data": source.get("data"),
            "protocol": source.get("protocol"),
            "candidate_count": source.get("candidate_count", 0),
            "eligible_count": source.get("eligible_count", 0),
            "selected": selected,
            "decision": source.get("decision", {}),
        },
        "laya_paper_telemetry": audit,
        "approval": {
            "historical_strategy_approved": False,
            "paper_trading_approved": False,
            "live_trading_approved": False,
            "reason": (
                "No candidate passed chronological development gates; the Laya paper log "
                "contains decisions but no simulated orders or fills."
            ),
        },
        "limitations": [
            "The reference replay ends at its source archive cutoff (2026-08-31), not 2026-09-23.",
            "The source replay uses minute aggregates and does not reconstruct a full L2 queue.",
            "Laya was observed in paper mode but was not historically replayed through the same C++ core.",
            "No profitability claim is made from a decision-only paper audit log.",
        ],
    }


def markdown(report: dict[str, Any]) -> str:
    replay = report["reference_replay"]
    decision = replay["decision"]
    audit = report["laya_paper_telemetry"]
    lines = [
        "# BTC threshold validation",
        "",
        "This is the first Astra research gate for BTCUSDT. It combines the existing causal",
        "aggTrade replay from `mmtick` with the current Laya paper-process telemetry. Large",
        "historical archives remain outside this repository and are referenced by manifest only.",
        "",
        "## Result",
        "",
        f"- **Decision:** `{decision.get('status', 'unknown')}`",
        f"- **Candidates:** `{replay.get('candidate_count', 0)}`; eligible after chronological gates: `{replay.get('eligible_count', 0)}`",
        "- **Paper exposure:** rejected",
        "- **Real/testnet execution:** unavailable by design",
        "",
        "No BTC threshold candidate was positive in both development windows under the stated",
        "cost model. The correct Astra action is `hold` until a replay through the C++ core",
        "produces a candidate with positive net out-of-sample performance and stable costs.",
        "",
        "## Replay protocol",
        "",
        f"- Data: `{replay.get('data', {}).get('minutes', 0):,}` one-minute aggregates, `{replay.get('data', {}).get('first')}` through `{replay.get('data', {}).get('last')}`.",
        "- Development: 2025-01-01 through 2025-12-31, split into train and validation.",
        "- Holdout: 2026 through the source archive cutoff; selection did not use holdout data.",
        "- Entry: a threshold event on a closed minute, entered at the next minute open.",
        "- Exit: first intraminute TP/SL touch; same-bar collision resolves to stop; otherwise close.",
        "- Costs: base 5 bps fee + 2 bps slippage per fill; stress 10 bps fee + 5 bps slippage per fill.",
        "",
        "## Laya paper telemetry",
        "",
        f"- Decisions observed: `{audit.get('events', 0):,}`; actions: `{audit.get('actions', {})}`.",
        f"- Simulated orders: `{audit.get('orders', 0)}`; fills: `{audit.get('fills', 0)}`.",
        f"- Inference latency summary (ms): `{audit.get('latency_ms', {})}`.",
        "",
        "This telemetry verifies that the sidecar and C++ decision path are connected. It is",
        "not a return series: without paper fills there is no defensible PnL, PF, drawdown,",
        "or win-rate estimate for Laya.",
        "",
        "## Next gate",
        "",
        "Implement the deterministic C++ replay adapter against the same feature and risk state",
        "used by live paper mode. Then replay the source event stream with configured inference,",
        "acknowledgement, and fill latency, and compare Laya with the no-model baseline under base",
        "and stress costs. Until that report passes, keep paper exposure disabled.",
        "",
        "Generated by `scripts/generate_btc_threshold_report.py`.",
        "",
    ]
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-report", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--paper-audit", type=Path, default=DEFAULT_AUDIT)
    parser.add_argument(
        "--output-dir", type=Path, default=Path("reports/btc_threshold_validation")
    )
    args = parser.parse_args()
    if not args.source_report.exists():
        raise SystemExit(f"source report does not exist: {args.source_report}")
    report = build(args.source_report, args.paper_audit)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    (args.output_dir / "README.md").write_text(markdown(report), encoding="utf-8")
    print(json.dumps(report["approval"], sort_keys=True))
    print(args.output_dir / "README.md")


if __name__ == "__main__":
    main()
