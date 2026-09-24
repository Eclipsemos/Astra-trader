# BTC label and feature diagnostics

This report scans the current raw aggregate-trade sample timestamps against the normalized
one-minute replay timeline. It does not change strategy parameters or approve paper trading.

## Findings

- The 60-minute, 12 bps round-trip, 2 bps margin label used by the baseline has enough directional
  observations, but its direction is weakly separated by the current features.
- At the 60-minute training label, the largest class effects are volatility, five-minute trade
  count, and spread proxy versus `hold` (roughly 0.43-0.60 standard deviations).
- Long versus short separation is near zero for every feature; the largest absolute effect is only
  about 0.08 standard deviations (`ret_15m`). This explains why the baseline can detect active
  periods but cannot choose a profitable direction reliably.
- Increasing the horizon to 240 minutes creates a much larger directional class, but that changes
  the strategy's holding period and must be tested as a new model rather than treated as a free
  improvement.
- Increasing assumed costs from 8 to 20 bps sharply reduces directional labels, so the label is
  sensitive to the execution-cost assumption.

## Diagnostic decision

Do not deepen Laya fine-tuning on the current 60-minute labels yet. The next model iteration should
either add genuinely directional features (multi-horizon order-flow persistence, signed returns,
trend state) or use a longer-horizon label and then be re-evaluated through C++ replay. Current
features mostly predict whether the market is active, not whether the next move is long or short.

Full 60-row horizon/cost grid and feature effect sizes are in `results.json`.
