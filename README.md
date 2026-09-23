# Astra Trader

A paper-only BTCUSDT decision trader with a C++ hot path, a local Laya inference sidecar, and
public Binance USD-M Futures market data.

The repository intentionally contains no Binance credentials, signed endpoints, testnet client,
or real order-submission path. Orders and fills exist only in the local `PaperExchange`.

## Build and test

```bash
bash scripts/build.sh
```

The build uses GCC 14, C++23, strict warnings, and Catch2. CMake and Ninja are pinned inside the
ignored `.tools` virtual environment.

## Run

Start Laya in one terminal:

```bash
bash scripts/run-laya.sh
```

Start the live paper trader in another:

```bash
bash scripts/run-paper.sh
```

Start the read-only dashboard:

```bash
bash scripts/run-dashboard.sh
```

Open `http://127.0.0.1:3000`. The C++ status API is available at
`http://127.0.0.1:8765/api/status`. The UI uses Bun when available and falls back to the same
Next.js/React toolchain through npm on hosts where Bun is not installed.

Start all three paper-only processes together with:

```bash
bash scripts/run-paper-stack.sh
```

For public-feed and paper-exchange diagnostics without Laya:

```bash
bash scripts/run-paper.sh --baseline
```

The process emits JSON lines for model decisions, simulated orders, fills, position, equity, and
latency. Stop it with `SIGINT` or `SIGTERM`.

## Research report

The first BTC threshold-validation gate is in
[reports/btc_threshold_validation/README.md](reports/btc_threshold_validation/README.md). It
records the cost-aware historical rejection, the source-data manifest, and the current Laya
paper telemetry. Rebuild the derived report with:

```bash
python3 scripts/generate_btc_threshold_report.py
```

The report deliberately does not copy the large mmtick history archives into this repository.

The C++ replay gate is generated from the cached mmtick minute stream with:

```bash
python3 scripts/build_btc_replay_csv.py \
  --cache /home/ldtdev/qt/mmtick/data/order_flow_cache/btc-1m-2025-20260831-profittaker.pkl \
  --output data/replay/btc-2025-2026.csv
python3 scripts/run_btc_cpp_replay_report.py --models hold,baseline,laya
```

Laya must be running on `127.0.0.1:8000` for the last command. The resulting C++ replay
report is in [reports/btc_cpp_replay_validation/README.md](reports/btc_cpp_replay_validation/README.md).

## Current limitations

- The live adapter currently consumes Binance `bookTicker`; aggregate trades and sequenced L2
  depth remain required before maker-fill research is credible.
- The included flow baseline is diagnostic only and has no profitability claim.
- The generic Laya checkpoint's emitted confidence is not calibrated for trading; the C++ gate
  currently requires 55% before a paper position can change.
- The host's NTP requests are currently timing out. Paper mode estimates a stable Binance clock
  offset and gates on adjusted event age; an offset reset or latency spike returns the gate to
  `hold`. A future real deployment must use a synchronized host clock instead.
- The status endpoint is read-only and intentionally exposes no order-control method.
- TSan compiles on this host but its runtime exits with `unexpected memory mapping`; ASan/UBSan
  and the normal test suites pass.
- Durable restart recovery and full L2 reconstruction remain in the implementation plan in
  [TODO.md](TODO.md). Aggregate replay is implemented, but its synthetic book cannot establish
  queue-level execution quality.
