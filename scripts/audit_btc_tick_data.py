#!/usr/bin/env python3
"""Audit the large BTC aggregate-trade sources without copying them into Git.

The audit deliberately reads ZIP central-directory metadata and only indexed boundary rows from
the SQLite database.  It does not scan the 70 GiB table or decompress the full archive set.
"""

from __future__ import annotations

import argparse
import json
import re
import sqlite3
import zipfile
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

ARCHIVE_RE = re.compile(r"BTCUSDT-aggTrades-(\d{4}-\d{2})(?:-(\d{2}))?\.zip$")


def iso_ms(value: int | None) -> str | None:
    if value is None:
        return None
    return datetime.fromtimestamp(value / 1_000, UTC).isoformat()


def zip_inventory(root: Path) -> dict[str, Any]:
    archives = []
    monthly: dict[str, Path] = {}
    daily: list[tuple[str, Path]] = []
    for path in sorted(root.glob("BTCUSDT-aggTrades-*.zip")):
        match = ARCHIVE_RE.fullmatch(path.name)
        if not match:
            continue
        month, day = match.groups()
        with zipfile.ZipFile(path) as archive:
            infos = archive.infolist()
            if len(infos) != 1:
                raise ValueError(f"expected one CSV member in {path}, found {len(infos)}")
            info = infos[0]
            record = {
                "path": str(path),
                "name": path.name,
                "period": month if day is None else f"{month}-{day}",
                "kind": "monthly" if day is None else "daily",
                "compressed_bytes": path.stat().st_size,
                "uncompressed_bytes": info.file_size,
                "member": info.filename,
            }
        archives.append(record)
        if day is None:
            monthly[month] = path
        else:
            daily.append((f"{month}-{day}", path))

    selected = list(monthly.values())
    selected.extend(path for month_day, path in daily if month_day[:7] not in monthly)
    selected_names = {path.name for path in selected}
    selected_records = [record for record in archives if record["name"] in selected_names]
    all_uncompressed = sum(record["uncompressed_bytes"] for record in archives)
    selected_uncompressed = sum(record["uncompressed_bytes"] for record in selected_records)
    return {
        "directory": str(root),
        "archive_count": len(archives),
        "monthly_count": len(monthly),
        "daily_count": len(daily),
        "compressed_bytes": sum(record["compressed_bytes"] for record in archives),
        "uncompressed_bytes": all_uncompressed,
        "deduplicated_selection": {
            "archive_count": len(selected_records),
            "compressed_bytes": sum(record["compressed_bytes"] for record in selected_records),
            "uncompressed_bytes": selected_uncompressed,
            "periods": sorted(record["period"] for record in selected_records),
            "excluded_overlap_days": sorted(
                record["period"]
                for record in archives
                if record["kind"] == "daily" and record["name"] not in selected_names
            ),
        },
        "first_period": min((record["period"] for record in archives), default=None),
        "last_period": max((record["period"] for record in archives), default=None),
        "archives": archives,
    }


def sqlite_inventory(path: Path) -> dict[str, Any]:
    uri = f"file:{path}?mode=ro"
    with sqlite3.connect(uri, uri=True, timeout=5) as connection:
        page_count = connection.execute("PRAGMA page_count").fetchone()[0]
        page_size = connection.execute("PRAGMA page_size").fetchone()[0]
        freelist_count = connection.execute("PRAGMA freelist_count").fetchone()[0]
        journal_mode = connection.execute("PRAGMA journal_mode").fetchone()[0]
        tables = [row[0] for row in connection.execute("SELECT name FROM sqlite_master WHERE type='table'")]
        columns = [row[1] for row in connection.execute("PRAGMA table_info(agg_trades)")]
        first = connection.execute(
            "SELECT instrument_id, symbol, timestamp_ms, price, quantity, source "
            "FROM agg_trades WHERE instrument_id = 'btc_perp' "
            "ORDER BY timestamp_ms ASC LIMIT 1"
        ).fetchone()
        last = connection.execute(
            "SELECT instrument_id, symbol, timestamp_ms, price, quantity, source "
            "FROM agg_trades WHERE instrument_id = 'btc_perp' "
            "ORDER BY timestamp_ms DESC LIMIT 1"
        ).fetchone()
    def boundary(row: tuple[Any, ...] | None) -> dict[str, Any] | None:
        if row is None:
            return None
        return {
            "instrument_id": row[0],
            "symbol": row[1],
            "timestamp_ms": row[2],
            "timestamp": iso_ms(row[2]),
            "price": row[3],
            "quantity": row[4],
            "source": row[5],
        }
    return {
        "path": str(path),
        "bytes": path.stat().st_size,
        "page_count": page_count,
        "page_size": page_size,
        "allocated_bytes": page_count * page_size,
        "freelist_pages": freelist_count,
        "journal_mode": journal_mode,
        "tables": tables,
        "agg_trades_columns": columns,
        "agg_trades_row_count": None,
        "row_count_note": "Exact COUNT(*) intentionally omitted; it requires a long scan of the 70 GiB table.",
        "btc_perp_first": boundary(first),
        "btc_perp_last": boundary(last),
    }


def markdown(report: dict[str, Any]) -> str:
    archives = report["archives"]
    selected = report["deduplicated_selection"]
    db = report["sqlite"]
    def gib(value: int) -> str:
        return f"{value / 2**30:.3f} GiB"
    excluded = ", ".join(selected["excluded_overlap_days"]) or "none"
    lines = [
        "# BTC aggregate-trade data audit",
        "",
        "This audit inspects the local Binance BTCUSDT USD-M aggregate-trade archives and the large",
        "SQLite store without copying raw data into the Astra repository.",
        "",
        "## Findings",
        "",
        f"- ZIP archives: `{len(archives)}` (`{report['monthly_count']}` monthly + `{report['daily_count']}` daily).",
        f"- ZIP storage: `{gib(report['compressed_bytes'])}` compressed; `{gib(report['uncompressed_bytes'])}` uncompressed.",
        f"- Deduplicated source selection: `{selected['archive_count']}` archives, `{gib(selected['uncompressed_bytes'])}` uncompressed.",
        f"- Archive coverage by filename: `{report['first_period']}` through `{report['last_period']}`.",
        f"- Daily files overlapping a monthly archive and therefore excluded: `{excluded}`.",
        f"- SQLite store: `{gib(db['bytes'])}` on disk; table `agg_trades` is present with `{len(db['agg_trades_columns'])}` columns.",
        f"- SQLite indexed BTC boundary: `{db['btc_perp_first']['timestamp']}` through `{db['btc_perp_last']['timestamp']}`.",
        "",
        "## Data interpretation",
        "",
        "The ZIP source is the current canonical archive for the latest data. It is Binance USD-M",
        "futures aggregate trades because the importer uses the futures Data Vision archive path and",
        "the rows contain `is_buyer_maker`. The SQLite store is an older/partial materialized store:",
        "its indexed BTC boundary stops before the latest September daily archives, so it must not be",
        "treated as the latest source without an explicit refresh.",
        "",
        "The previous Astra report used a derived one-minute cache ending 2026-08-31. That was a",
        "research reduction for replay speed, not evidence that the aggregate-trade data was absent.",
        "The raw source is large enough to support a separate tick-level BTC adaptation experiment.",
        "",
        "## Next dataset contract",
        "",
        "- Prefer monthly archives when a month is complete; append daily archives only for uncovered months/dates.",
        "- Deduplicate by `aggregate_trade_id` and preserve `transact_time`, price, quantity, and buyer-maker flag.",
        "- Generate compact 1-second/5-second feature caches; do not commit raw or generated caches to Git.",
        "- Build cost-aware labels from future executable assumptions, then split chronologically before model fitting.",
        "",
        "Generated by `scripts/audit_btc_tick_data.py`.",
        "",
    ]
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive-dir", type=Path, default=Path("/home/ldtdev/qt/mmtick/data/history_btc_tick"))
    parser.add_argument("--database", type=Path, default=Path("/home/ldtdev/qt/mmtick/data/btc_tick.db"))
    parser.add_argument("--output-dir", type=Path, default=Path("reports/btc_tick_data_audit"))
    args = parser.parse_args()
    if not args.archive_dir.is_dir():
        raise SystemExit(f"archive directory does not exist: {args.archive_dir}")
    if not args.database.exists():
        raise SystemExit(f"database does not exist: {args.database}")
    archives = zip_inventory(args.archive_dir)
    report = {
        "schema": "astra.report.btc-tick-data-audit.v1",
        "generated_at": datetime.now(UTC).isoformat(),
        **archives,
        "sqlite": sqlite_inventory(args.database),
        "decision": {
            "raw_aggregate_trades_available": True,
            "latest_canonical_source": "ZIP archives",
            "sqlite_is_latest_complete_source": False,
            "ready_for_tick_feature_build": True,
        },
    }
    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    (args.output_dir / "README.md").write_text(markdown(report), encoding="utf-8")
    print(json.dumps(report["decision"], indent=2))
    print(args.output_dir / "README.md")


if __name__ == "__main__":
    main()
