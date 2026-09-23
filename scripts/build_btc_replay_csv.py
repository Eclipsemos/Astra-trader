#!/usr/bin/env python3
"""Convert mmtick BTC aggTrades into the normalized Astra replay stream.

The output is intentionally generated under ``data/`` (which is ignored by Git). It is a
minute aggregate with a synthetic top-of-book around the minute close. This is suitable for
causal feature/risk/paper-state validation, not for claiming full-L2 execution quality.
"""

from __future__ import annotations

import argparse
import csv
import io
import json
from dataclasses import dataclass
from datetime import UTC, datetime
from decimal import ROUND_HALF_UP, Decimal
from pathlib import Path
from zipfile import ZipFile

ROOT = Path("/home/ldtdev/qt/mmtick/data/history_btc_tick")
START_MS = int(datetime(2025, 1, 1, tzinfo=UTC).timestamp() * 1000)
END_MS = int(datetime(2026, 8, 31, 23, 59, 59, tzinfo=UTC).timestamp() * 1000)


@dataclass
class Minute:
    timestamp_ms: int
    open_price: Decimal
    close_price: Decimal
    buy_notional: Decimal = Decimal(0)
    sell_notional: Decimal = Decimal(0)


@dataclass
class TickMinute:
    """Pickle-compatible shape used by mmtick's existing 1-minute cache."""

    timestamp_ms: int
    open: Decimal
    high: Decimal
    low: Decimal
    close: Decimal
    notional: Decimal
    reported_buy: Decimal
    reported_sell: Decimal
    tick_buy: Decimal
    tick_sell: Decimal


def archive_paths(source: Path, end_ms: int) -> list[Path]:
    monthly = sorted(
        path
        for path in source.glob("BTCUSDT-aggTrades-*.zip")
        if len(path.stem.split("-")) == 4
    )
    covered = {"-".join(path.stem.split("-")[2:4]) for path in monthly}
    daily = sorted(
        path
        for path in source.glob("BTCUSDT-aggTrades-*.zip")
        if len(path.stem.split("-")) == 5
        and "-".join(path.stem.split("-")[2:4]) not in covered
    )
    selected = monthly + daily
    result: list[Path] = []
    for path in selected:
        # Keep the source selection deterministic and bound it to the requested date range.
        if "2025" not in path.name and "2026" not in path.name:
            continue
        if end_ms < START_MS:
            continue
        result.append(path)
    return result


def price_units(value: Decimal) -> int:
    return int((value * 100).quantize(Decimal(1), rounding=ROUND_HALF_UP))


def aggregate(path: Path, end_ms: int) -> list[Minute]:
    grouped: dict[int, Minute] = {}
    with ZipFile(path) as archive:
        name = next(item for item in archive.namelist() if item.endswith(".csv"))
        reader = csv.DictReader(io.TextIOWrapper(archive.open(name), encoding="utf-8"))
        for row in reader:
            timestamp_ms = int(row["transact_time"])
            if timestamp_ms < START_MS or timestamp_ms > end_ms:
                continue
            bucket = timestamp_ms // 60_000 * 60_000
            price = Decimal(row["price"])
            quantity = Decimal(row["quantity"])
            item = grouped.get(bucket)
            if item is None:
                item = Minute(bucket, price, price)
                grouped[bucket] = item
            item.close_price = price
            notional = price * quantity
            if row["is_buyer_maker"].lower() == "false":
                item.buy_notional += notional
            else:
                item.sell_notional += notional
    return [grouped[key] for key in sorted(grouped)]


def write_stream(source: Path, output: Path, end_ms: int, cache: Path | None) -> dict[str, object]:
    output.parent.mkdir(parents=True, exist_ok=True)
    paths = archive_paths(source, end_ms) if cache is None else []
    sequence = 0
    minutes = 0
    first_ms: int | None = None
    last_ms: int | None = None
    with output.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(
            [
                "sequence",
                "exchange_time_ns",
                "best_bid_units",
                "best_ask_units",
                "bid_quantity_units",
                "ask_quantity_units",
                "trade_price_units",
                "trade_quantity_units",
                "aggressor_side",
            ]
        )
        if cache is not None:
            import pickle

            print(f"reading cache {cache}", flush=True)
            with cache.open("rb") as handle:
                cached = pickle.load(handle)
            source_minutes = [
                Minute(item.timestamp_ms, item.open, item.close, item.reported_buy, item.reported_sell)
                for item in cached
                if START_MS <= item.timestamp_ms <= end_ms
            ]
            archive_items = [(cache, source_minutes)]
        else:
            archive_items = []
            for index, path in enumerate(paths, start=1):
                print(f"[{index}/{len(paths)}] {path.name}", flush=True)
                archive_items.append((path, aggregate(path, end_ms)))
        for path, items in archive_items:
            for item in items:
                close_units = price_units(item.close_price)
                # Two-sided synthetic spread: 1 bp on each side of the minute close.
                half_spread = max(1, close_units // 100_000)
                total = item.buy_notional + item.sell_notional
                imbalance = 0
                if total > 0:
                    imbalance = int(
                        ((item.buy_notional - item.sell_notional) / total * 1_000_000)
                        .quantize(Decimal(1), rounding=ROUND_HALF_UP)
                    )
                imbalance = max(-900_000, min(900_000, imbalance))
                sequence += 1
                writer.writerow(
                    [
                        sequence,
                        item.timestamp_ms * 1_000_000,
                        close_units - half_spread,
                        close_units + half_spread,
                        1_000_000 + imbalance,
                        1_000_000 - imbalance,
                        close_units,
                        1,
                        "buy" if imbalance >= 0 else "sell",
                    ]
                )
                minutes += 1
                first_ms = item.timestamp_ms if first_ms is None else first_ms
                last_ms = item.timestamp_ms
    manifest = {
        "schema": "astra.replay.input.v1",
        "source": str(source),
        "archives": [str(path) for path in paths] if cache is None else [],
        "cache": str(cache) if cache is not None else None,
        "start": datetime.fromtimestamp(first_ms / 1000, tz=UTC).isoformat()
        if first_ms is not None
        else None,
        "last": datetime.fromtimestamp(last_ms / 1000, tz=UTC).isoformat()
        if last_ms is not None
        else None,
        "minutes": minutes,
        "synthetic_book": True,
        "trade_quantity_units": 1,
    }
    manifest_path = output.with_suffix(".manifest.json")
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT)
    parser.add_argument("--output", type=Path, default=Path("data/replay/btc-2025-2026.csv"))
    parser.add_argument("--end-ms", type=int, default=END_MS)
    parser.add_argument("--cache", type=Path, default=None)
    args = parser.parse_args()
    manifest = write_stream(args.source, args.output, args.end_ms, args.cache)
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
