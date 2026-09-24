# BTC causal regime-router OOS validation

This experiment keeps the original BTC softmax direction model and adds a causal market-state
gate. Regimes are derived from 60-minute trend efficiency, return direction, and train-period
volatility quartiles. The gate and probability threshold are selected only on 2025 validation by
the production C++ base-cost replay; 2026 is untouched until final evaluation.

## Selected router

- Allowed state: all `trend_up` and `trend_down` volatility states; reject `range` states.
- Direction probability threshold: `0.50`.
- 2025 validation: `+4.3706%`, PF `1.913`, 62 fills.

| Period | Base return | Base PF | Base fills | Stress return | Stress PF |
|---|---:|---:|---:|---:|---:|
| 2026 aggregate | +0.2122% | 1.100 | 132 | -1.4582% | 1.029 |
| 2026 Q1 | +1.5279% | 1.402 | 68 | +0.6401% | 1.317 |
| 2026 Q2 | -0.3029% | 1.006 | 52 | -0.9278% | 0.927 |
| 2026 Jul-Aug | -0.9364% | 0.470 | 12 | -1.0828% | 0.448 |

## Robustness decision

Reject for paper trading. The aggregate base-cost result is slightly positive, but the edge fails
stress costs and decays after Q1. The selected threshold is also an isolated point: neighboring
trend thresholds `0.42` and `0.45` lose `3.6220%` and `2.9645%` respectively on validation. This is
not a stable parameter platform and is exposed to validation-selection bias.

Full candidate, tape, aggregate, and subperiod evidence is in `results.json`.

