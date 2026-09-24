#!/usr/bin/env python3
"""Build a compact causal 5-second BTC feature cache from Binance aggTrades ZIPs.

Raw archives stay under mmtick.  The output is gzip CSV and a manifest; neither is intended for
Git.  Monthly archives take precedence over overlapping daily files so August 2026 is not double
counted, while the uncovered September daily files are appended.
"""

from __future__ import annotations

import argparse
import csv
import gzip
import hashlib
import io
import json
import zipfile
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

from audit_btc_tick_data import zip_inventory


FIELDS = (
    "timestamp_ms",
    "open",
    "high",
    "low",
    "close",
    "notional",
    "buy_notional",
    "sell_notional",
    "trade_count",
    "buy_count",
    "sell_count",
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def selected_archives(root: Path) -> list[Path]:
    inventory = zip_inventory(root)
    selected = set(inventory["deduplicated_selection"]["periods"])
    return [Path(row["path"]) for row in inventory["archives"] if row["period"] in selected]


def emit_bar(writer: csv.writer, bucket: list[Any] | None) -> int:
    if bucket is None:
        return 0
    writer.writerow(bucket)
    return 1


def build(archives: list[Path], output: Path) -> dict[str, Any]:
    output.parent.mkdir(parents=True, exist_ok=True)
    rows = 0
    trades = 0
    first_ms = None
    last_ms = None
    bucket: list[Any] | None = None
    with gzip.open(output, "wt", newline="", encoding="utf-8", compresslevel=5) as handle:
        writer = csv.writer(handle)
        writer.writerow(FIELDS)
        for archive_path in archives:
            print(f"reading {archive_path.name}", flush=True)
            with zipfile.ZipFile(archive_path) as archive:
                member = archive.infolist()[0]
                reader = csv.DictReader(io.TextIOWrapper(archive.open(member), encoding="utf-8"))
                for record in reader:
                    timestamp_ms = int(record["transact_time"])
                    price = record["price"]
                    quantity = float(record["quantity"])
                    notional = float(price) * quantity
                    current_bucket = timestamp_ms // 5_000 * 5_000
                    if bucket is None or current_bucket != bucket[0]:
                        rows += emit_bar(writer, bucket)
                        bucket = [current_bucket, price, price, price, price, notional, 0.0, 0.0, 1, 0, 0]
                    else:
                        bucket[2] = max(bucket[2], price, key=float)
                        bucket[3] = min(bucket[3], price, key=float)
                        bucket[4] = price
                        bucket[5] += notional
                        bucket[8] += 1
                    if record["is_buyer_maker"].lower() == "false":
                        bucket[6] += notional
                        bucket[9] += 1
                    else:
                        bucket[7] += notional
                        bucket[10] += 1
                    trades += 1
                    first_ms = timestamp_ms if first_ms is None else min(first_ms, timestamp_ms)
                    last_ms = timestamp_ms if last_ms is None else max(last_ms, timestamp_ms)
        rows += emit_bar(writer, bucket)
    return {
        "output": str(output),
        "sha256": sha256(output),
        "bytes": output.stat().st_size,
        "bars": rows,
        "trades": trades,
        "first_trade": datetime.fromtimestamp(first_ms / 1000, UTC).isoformat() if first_ms else None,
        "last_trade": datetime.fromtimestamp(last_ms / 1000, UTC).isoformat() if last_ms else None,
        "cadence_ms": 5_000,
        "fields": list(FIELDS),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive-dir", type=Path, default=Path("/home/ldtdev/qt/mmtick/data/history_btc_tick"))
    parser.add_argument("--output", type=Path, default=Path("/home/ldtdev/qt/mmtick/data/order_flow_cache/btc-5s-aggtrade-v1.csv.gz"))
    parser.add_argument("--manifest", type=Path, default=Path("reports/btc_tick_feature_build/manifest.json"))
    args = parser.parse_args()
    archives = selected_archives(args.archive_dir)
    if not archives:
        raise SystemExit("no selected archives")
    result = build(archives, args.output)
    manifest = {
        "schema": "astra.manifest.btc-aggtrade-5s.v1",
        "generated_at": datetime.now(UTC).isoformat(),
        "source": {"directory": str(args.archive_dir), "archives": [str(path) for path in archives]},
        "feature_cache": result,
        "selection": "monthly archives preferred; uncovered daily archives appended",
        "deduplication": "one monthly or daily archive per covered period; aggregate_trade_id de-duplication is implicit in Binance archive selection",
        "paper_approved": False,
    }
    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    args.manifest.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
