# Astra Trader TODO

## Goal

Build a low-latency, decision-driven trading system for Binance USD-M Futures. The market-data,
feature, risk, execution, accounting, persistence, and replay backend will be written in modern
C++. Laya remains a local Python inference service because its runtime is based on PyTorch. The
frontend and control plane will use the Bun/Next.js/TypeScript stack established by `jev-trader`.

Reuse validated behavior and data contracts from `mmtick`, but do not put Python in the trading
hot path except for the explicitly bounded Laya inference call. Keep every model decision behind
deterministic C++ execution and risk controls.

Version 0.1 ends at simulation. It consumes public Binance market data but does not connect to
authenticated Binance endpoints, Binance testnet, or any real order-entry API.

Initial scope:

- Symbol: `BTCUSDT`
- Market-data source: Binance USD-M Futures public APIs
- Backend: C++20 or newer, built with CMake
- Frontend: Next.js, React, TypeScript, and Bun
- Model runtime: preloaded Laya Python sidecar on the same host
- Runtime: historical replay, live shadow, and local paper execution only
- Explicitly out of scope: Binance testnet, authenticated account APIs, and real order submission
- Model actions: `long`, `short`, or `hold`
- Primary development directory: `/home/ldtdev/qt/mmAstra/astra-trader`
- Reference projects:
  - `/home/ldtdev/qt/mmAstra/jev-trader`
  - `/home/ldtdev/qt/mmAstra/laya`
  - `/home/ldtdev/qt/mmtick`
  - `/home/ldtdev/qt/mmcpp`

## Target Architecture

The C++ process owns all authoritative simulated trading state. The Laya service proposes
decisions, and the frontend observes state; neither is allowed to mutate the paper ledger directly.

```text
Binance market WebSockets
          |
          v
C++ market I/O -> local L2 book -> feature/state builder
                                      |
                                      v
                              bounded Laya IPC
                                      |
                                      v
                         C++ decision validity gate
                                      |
                                      v
                 C++ risk engine -> paper exchange simulator
                                      |              |
                                      ^              v
                                order state <- simulated fills

C++ event stream -> read-only API/SSE -> Bun/Next.js frontend
C++ journal writer <- bounded off-hot-path event queue
```

Threading model:

- Market I/O executor: WebSocket receive, decode, timestamp, and sequence validation.
- Core state thread: single writer for book publication, features, decisions, risk, orders,
  positions, and PnL. No shared mutable trading state across threads.
- Model IPC worker: coalesces obsolete states, enforces one bounded deadline, and returns a
  versioned response to the core thread.
- Replay/data worker: streams historical events without changing the live core state contract.
- Journal/telemetry worker: receives immutable events through a bounded queue; disk and UI work
  never block the core state thread.

Use bounded SPSC/MPSC queues and explicit overload policies. Consider CPU affinity, allocator
tuning, lock-free structures, and kernel/network tuning only after profiling proves they matter.

Initial C++ dependency candidates (to validate in Phase 0):

- Boost.Asio/Beast or an equivalently maintained asynchronous TLS/WebSocket stack
- OpenSSL for TLS support
- simdjson or RapidJSON for bounded JSON parsing
- SQLite in WAL mode for the durable journal and recovery state
- GoogleTest or Catch2 for unit and integration tests
- Google Benchmark or an internal monotonic-clock benchmark harness

## Non-Negotiable Scope and Safety Rules

- [x] Do not implement real or testnet order submission in Version 0.1.
- [x] Do not implement signed Binance endpoints or load Binance API credentials.
- [x] Never store passwords, API keys, secrets, account responses, or databases in Git.
- [x] The model must never submit orders or choose unconstrained order size directly.
- [ ] Stale market data, an order-book sequence gap, model timeout, or paper-state recovery failure
      must produce `hold` and block new simulated exposure.
- [x] Every simulated order must have a deterministic and unique order ID.
- [ ] Restart recovery must restore the paper ledger, simulated orders, fills, and strategy state.
- [x] There must be no configuration switch that can turn the simulator into a real exchange
      client.

## Phase 0: C++ Foundation, Contracts, and Baselines

- [ ] Create the C++ source layout, CMake project, dependency lock/manifest, presets, warning
      policy, sanitizers, formatter, static analysis, and test layout.
- [x] Install or provision CMake; this host currently has GCC 14 but no `cmake` executable. Never
      write sudo credentials into project files, shell history, scripts, or logs.
- [ ] Write an architecture decision record for the single-writer C++ core, Laya sidecar, and
      separate Bun/Next.js frontend.
- [ ] Decide whether to reuse selected `mmcpp` libraries or copy only stable interfaces and tests.
- [x] Add debug, release, ASan/UBSan, and TSan build presets.
- [ ] Define normalized event types for trades, book ticker, depth updates, mark price, orders,
      fills, positions, decisions, and health events.
- [x] Use fixed-point integer types for all prices, quantities, fees, balances, and PnL.
- [ ] Define the model input schema with explicit event time, receive time, state age, and horizon.
- [ ] Define the model output schema: action, probabilities/confidence, horizon, model version, and
      inference latency.
- [ ] Define monotonic-clock and exchange-clock semantics for latency measurements.
- [x] Implement a deterministic no-model baseline for comparison.
- [ ] Record measurable latency and research targets; do not label the system HFT based only on
      inference speed.

Exit criteria:

- Schemas are versioned and validated.
- A recorded event can pass through the whole pipeline without an exchange connection.
- Release, sanitizer, and unit-test builds pass.
- Paper mode is the only available execution mode.

## Phase 1: Binance Market Data

- [x] Implement public Binance USD-M Futures WebSocket connectivity in C++.
- [ ] Consume aggregate trades, best bid/ask, incremental depth, and mark-price streams.
- [ ] Build the local L2 book using a REST snapshot plus buffered depth updates.
- [ ] Validate update IDs and rebuild the book after gaps or reconnects.
- [x] Detect stale streams, crossed books, invalid prices, and clock drift.
- [ ] Add bounded queues and an explicit overload policy so stale events are not processed late.
- [ ] Parse into preallocated/fixed-capacity structures where practical and measure allocations in
      the hot path.
- [ ] Record exchange event time, local receive time, processing time, and sequence identifiers.
- [ ] Add reconnect/backoff tests and fixture-based protocol tests.

Exit criteria:

- The local best bid/ask agrees with Binance snapshots during a bounded observation run.
- Injected gaps and reconnects reliably disable decisions until recovery completes.
- Feed latency p50/p95/p99 and dropped-event counts are observable.

## Phase 2: Laya Decision Sidecar and C++ Gateway

- [x] Run one pinned Laya checkpoint in a preloaded local Python sidecar.
- [x] Implement an asynchronous C++ model gateway using a persistent localhost HTTP connection.
- [ ] Benchmark localhost HTTP against a Unix domain socket before selecting a production transport.
- [x] Version the request/response protocol and reject unknown or incomplete responses.
- [ ] Benchmark cold start, warm inference, memory use, throughput, and p50/p95/p99 latency on the
      current CPU-only host.
- [x] Represent market state compactly without locale-dependent formatting or free-form ambiguity.
- [x] Add `long`, `short`, and `hold` as first-class choices.
- [x] Add strict inference deadlines and return `hold` on timeout or model error.
- [x] Attach a monotonically increasing state sequence and reject responses for superseded states.
- [x] Coalesce pending work so model latency cannot create an unbounded queue of stale decisions.
- [ ] Record the checkpoint hash, schema version, input hash, raw probabilities, and routing result.
- [ ] Test determinism and concurrency under the intended event rate.
- [x] Decide whether the generic checkpoint is adequate or market-data fine-tuning is required.

Exit criteria:

- No checkpoint is loaded lazily in the hot path.
- The C++ core remains operational and safely returns `hold` if the sidecar is unavailable.
- The model meets the agreed latency budget under sustained load.
- Identical replay input produces auditable, reproducible output.

## Phase 3: Historical Data and Replay

- [x] Inventory `mmtick` BTC data by venue, market type, date, schema, cadence, and completeness.
- [x] Confirm the full Binance USD-M aggregate-trade source (92.1 GiB uncompressed after monthly/daily
      de-duplication through 2026-09-14); see `reports/btc_tick_data_audit/`.
- [x] Import aggregate trades without duplicating the large source datasets.
- [ ] Import historical book-depth curves and clearly distinguish them from full L2 data.
- [ ] Import the available approximately 13-hour full-depth capture for microstructure validation.
- [x] Build the causal aggregate replay engine in C++ and run it through the same core state
      machine used by live market data.
- [x] Add a C++ decision-tape replay path so Python/Laya training output is evaluated by the
      production fixed-point paper ledger; see `reports/btc_laya_head_oos/`.
- [ ] Add configurable inference, network, order acknowledgement, cancel, and fill latency.
- [x] Prevent future data leakage in rolling features, labels, queue state, and fills.
- [ ] Reproduce selected prior `mmtick` HFT results as a regression check.

Exit criteria:

- Replay is deterministic across repeated runs.
- A slower replay mode can validate every state transition and timestamp.
- Data limitations are reported rather than silently filled or interpolated.

## Phase 4: Model Research

- [ ] Select prediction horizons only after measuring the achievable live latency distribution.
- [x] Define cost-aware labels based on executable bid/ask prices, not mid-price direction alone;
      see `reports/btc_model_oos_validation/`.
- [x] Use chronological train, validation, and untouched holdout periods.
- [x] Compare the frozen Laya checkpoint against a BTC-specific linear softmax/logistic baseline;
      the tree/boosting extension remains open.
- [x] Calibrate action probabilities and require the research label to clear fees, spread,
      slippage, and a configurable safety margin; production expected-edge gating remains open.
- [ ] Evaluate results by regime, volatility, liquidity, hour, direction, and holding horizon.
- [ ] Penalize unstable parameter points and prefer broad performance plateaus.
- [x] Reject the current model/baseline configuration when it does not improve net out-of-sample
      results over the control; see `reports/btc_cpp_replay_validation/`.

Minimum research report:

- Gross and net return
- Profit factor and maximum drawdown
- Trade count and exposure
- Maker/taker mix and fill rate
- Adverse selection after fills
- Turnover, fees, slippage, and latency sensitivity
- Annual/monthly and regime-level stability
- Confidence intervals or bootstrap uncertainty

## Phase 5: Paper Exchange and Position State Machine

- [ ] Implement deterministic market, limit, post-only, cancel, and cancel/replace simulation.
- [x] Implement configurable maker/taker fees, spread, slippage, acknowledgement latency, cancel
      latency, queue-ahead, partial fills, and adverse same-timestamp ordering.
- [ ] Drive simulated fills only from causally subsequent public market events.
- [ ] Support partial fills, cancel/fill races, simulated rejects, expirations, and duplicate input
      events.
- [ ] Persist simulated order intent, acknowledgement, lifecycle transitions, and fills.
- [x] Use checked fixed-point integer arithmetic for quantities, prices, fees, and PnL; do not use
      binary floating point in execution or accounting paths.
- [ ] Load public exchange filters and enforce quantity steps, price ticks, minimum notional, and
      configured paper position mode.
- [ ] Add restart, duplicate-event, and cancel/fill race recovery tests.

Exit criteria:

- Paper execution survives feed disconnects, duplicates, partial fills, and process restarts.
- Replay and live-paper modes produce the same transitions for the same ordered event stream.
- Reprocessing an event cannot create a duplicate logical order or fill.

## Phase 6: Deterministic Risk Layer

- [ ] Enforce maximum position, order notional, leverage, and open-order count.
- [ ] Enforce daily loss, drawdown, turnover, order-rate, and consecutive-loss limits.
- [ ] Add spread, liquidity, volatility, stale-data, and model-confidence gates.
- [ ] Add reduce-only liquidation and manual flatten operations.
- [ ] Add persistent pause state and a kill switch independent of Laya.
- [ ] Reject new simulated exposure when paper ledger, market, or model state is uncertain.
- [ ] Test long/short symmetry and every risk rejection reason.

## Phase 7: Observability and Operations

- [ ] Persist market-state hashes, features, decisions, order intents, acknowledgements, fills, PnL,
      errors, and latency spans.
- [x] Expose health, readiness, positions, orders, decisions, and risk status from the C++ backend
      through a read-only HTTP/SSE API.
- [x] Port the useful `jev-trader/web` patterns to an Astra-specific Next.js frontend compatible
      with Bun and npm.
- [ ] Generate or validate TypeScript wire types from the versioned C++ API schema.
- [x] Keep dashboard and logging work outside the decision hot path.
- [ ] Add metrics for queue depth, dropped events, reconnects, book rebuilds, inference errors, order
      rejects, paper-ledger drift, and latency percentiles.
- [ ] Provide a scrubbed diagnostic bundle that cannot contain secrets or account data.
- [ ] Add graceful shutdown that cancels or preserves simulated orders according to an explicit
      policy.

## Phase 8: Shadow and Paper Validation

- [ ] Run shadow mode without submitting orders for at least seven continuous days.
- [ ] Compare predicted fills with subsequent market data and measure simulation bias.
- [ ] Stress test stream disconnects, delayed inference, clock drift, rate limits, and REST failures.
- [ ] Produce a simulation acceptance/rejection report from untouched forward data.

Version 0.1 acceptance requires:

- Positive net forward result after realistic costs.
- Acceptable drawdown and loss-tail behavior.
- Sufficient independent trades across more than one market regime.
- Stable results under worse latency and cost assumptions.
- No unresolved state divergence or safety-critical test failure.

## Explicitly Out of Scope

- Binance API keys or authenticated account access
- Binance testnet connectivity
- Signed order submission, cancellation, or account reconciliation
- Real capital, deposits, withdrawals, leverage changes, or live position management
- Any claim that simulated profitability proves live executability

## Initial Decisions to Confirm

- [x] Confirm `BTCUSDT` USD-M perpetual as the first and only instrument.
- [ ] Confirm whether the intended execution style is maker, taker, or selectively mixed.
- [ ] Confirm the target decision horizon and acceptable end-to-end latency class.
- [ ] Confirm whether a CUDA-capable deployment host will be available.
- [ ] Confirm simulated position mode, paper leverage, and paper risk limits.
- [x] Use the existing `jev-trader/web` technology stack: Next.js, React, TypeScript, and Bun.
- [x] Confirm which dashboard components should be ported after the backend event schema is stable.

## Definition of Done for Version 0.1

- A single command builds and starts the C++ backend, Laya sidecar, and frontend in paper-only mode.
- Recorded BTCUSDT events can be replayed deterministically through the same production pipeline.
- Laya and baseline models can be evaluated side by side with identical inputs and costs.
- All decisions, risk outcomes, simulated orders, and fills are auditable.
- The system reports honest latency and data-quality metrics.
- The C++ hot path has benchmark coverage and passes ASan/UBSan; concurrent components pass TSan.
- The repository contains no authenticated Binance client or real/testnet order path.
