#!/usr/bin/env python3
"""Create causal BTC model samples from the streamed 5-second feature cache."""

from __future__ import annotations

import argparse
import csv
import gzip
import hashlib
import json
import math
from array import array
from datetime import UTC, datetime
from pathlib import Path

FEATURES = ("ret_1m", "ret_5m", "ret_15m", "ret_60m", "vol_60m", "flow_5m", "trades_5m", "spread_proxy")
HEADER = ("sequence", "timestamp_ms", *FEATURES, "label")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def split(timestamp_ms: int) -> str | None:
    stamp = datetime.fromtimestamp(timestamp_ms / 1000, UTC)
    if stamp < datetime(2025, 1, 1, tzinfo=UTC):
        return None
    if stamp < datetime(2025, 7, 1, tzinfo=UTC):
        return "development_train"
    if stamp < datetime(2026, 1, 1, tzinfo=UTC):
        return "development_validation"
    if stamp < datetime(2026, 9, 15, tzinfo=UTC):
        return "holdout_2026"
    return None


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=Path("/home/ldtdev/qt/mmtick/data/order_flow_cache/btc-5s-aggtrade-v1.csv.gz"))
    parser.add_argument("--output", type=Path, default=Path("/home/ldtdev/qt/mmtick/data/order_flow_cache/btc-laya-1m-samples-v1.csv.gz"))
    parser.add_argument("--manifest", type=Path, default=Path("reports/btc_tick_feature_build/laya_samples_manifest.json"))
    parser.add_argument("--horizon-minutes", type=int, default=60)
    args = parser.parse_args()
    if not args.input.exists():
        raise SystemExit(f"missing feature cache: {args.input}")
    closes = array("d")
    flows = array("d")
    trade_counts = array("d")
    spreads = array("d")
    timestamps = array("q")
    current_minute = None
    minute_close = minute_buy = minute_sell = minute_trades = minute_high = minute_low = None
    with gzip.open(args.input, "rt", newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        for row in reader:
            timestamp_ms = int(row["timestamp_ms"])
            minute = timestamp_ms // 60_000 * 60_000
            if current_minute is None:
                current_minute = minute
                minute_close = float(row["close"])
                minute_high = float(row["high"])
                minute_low = float(row["low"])
                minute_buy = float(row["buy_notional"])
                minute_sell = float(row["sell_notional"])
                minute_trades = int(row["trade_count"])
            elif minute != current_minute:
                total = minute_buy + minute_sell
                timestamps.append(current_minute)
                closes.append(minute_close)
                flows.append((minute_buy - minute_sell) / total if total else 0.0)
                trade_counts.append(minute_trades)
                spreads.append((minute_high - minute_low) / minute_close if minute_close else 0.0)
                current_minute = minute
                minute_close = float(row["close"])
                minute_high = float(row["high"])
                minute_low = float(row["low"])
                minute_buy = float(row["buy_notional"])
                minute_sell = float(row["sell_notional"])
                minute_trades = int(row["trade_count"])
            else:
                minute_close = float(row["close"])
                minute_high = max(minute_high, float(row["high"]))
                minute_low = min(minute_low, float(row["low"]))
                minute_buy += float(row["buy_notional"])
                minute_sell += float(row["sell_notional"])
                minute_trades += int(row["trade_count"])
        if current_minute is not None:
            total = minute_buy + minute_sell
            timestamps.append(current_minute)
            closes.append(minute_close)
            flows.append((minute_buy - minute_sell) / total if total else 0.0)
            trade_counts.append(minute_trades)
            spreads.append((minute_high - minute_low) / minute_close if minute_close else 0.0)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    counts: dict[str, int] = {}
    rows = 0
    with gzip.open(args.output, "wt", newline="", encoding="utf-8", compresslevel=5) as handle:
        writer = csv.writer(handle)
        writer.writerow(HEADER)
        for index in range(60, len(timestamps) - args.horizon_minutes, 60):
            period = split(timestamps[index])
            if period is None:
                continue
            future = closes[index + args.horizon_minutes]
            current = closes[index]
            long_edge = future / current - 1.0 - 0.0012
            short_edge = current / future - 1.0 - 0.0012
            label = 1 if long_edge > 0.0002 and long_edge >= short_edge else 2 if short_edge > 0.0002 else 0
            returns = [math.log(closes[index] / closes[index - lookback]) for lookback in (1, 5, 15, 60)]
            returns_60m = [math.log(closes[index - offset] / closes[index - offset - 1]) for offset in range(60)]
            mean_return = sum(returns_60m) / len(returns_60m)
            volatility = math.sqrt(sum((value - mean_return) ** 2 for value in returns_60m) / max(1, len(returns_60m) - 1))
            flow = sum(flows[index - offset] for offset in range(5)) / 5
            trades = sum(trade_counts[index - offset] for offset in range(5))
            row = [index, timestamps[index], *returns, volatility, flow, trades, spreads[index], label]
            writer.writerow(row)
            rows += 1
            counts[period] = counts.get(period, 0) + 1
    manifest = {
        "schema": "astra.manifest.btc-laya-tick-samples.v1",
        "generated_at": datetime.now(UTC).isoformat(),
        "input": {"path": str(args.input), "sha256": sha256(args.input), "one_minute_bars": len(timestamps)},
        "output": {"path": str(args.output), "sha256": sha256(args.output), "bytes": args.output.stat().st_size, "samples": rows, "split_samples": counts, "horizon_minutes": args.horizon_minutes},
        "label": "future close executable-cost proxy: 12 bps round trip plus 2 bps safety margin",
        "paper_approved": False,
    }
    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    args.manifest.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(manifest["output"], indent=2))


if __name__ == "__main__":
    main()
