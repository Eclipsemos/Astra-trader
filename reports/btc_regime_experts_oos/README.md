# BTC regime-expert strategy set OOS validation

This is the first explicit strategy-set test. It trains independent class-weighted direction
experts for causal `trend_up`, `trend_down`, and `range` states, then automatically routes each
decision to the matching expert. Training uses 2025 H1; the common action threshold is selected by
2025 H2 C++ replay; 2026 remains holdout.

## Expert samples

| Expert | Training rows |
|---|---:|
| trend up | 893 |
| trend down | 845 |
| range | 2,606 |

The best eligible validation threshold was `0.60`, but validation itself was already negative:
`-0.2658%`, PF `1.015`, 40 fills.

| Period | Base return | Base PF | Base fills | Stress return | Stress PF |
|---|---:|---:|---:|---:|---:|
| 2026 aggregate | -4.0693% | 0.631 | 80 | -5.0788% | 0.594 |
| 2026 Q1 | -3.3756% | 0.531 | 57 | -4.1060% | 0.495 |
| 2026 Q2 | +0.8202% | 1.806 | 19 | +0.6000% | 1.704 |
| 2026 Jul-Aug | -1.8763% | 0.028 | 5 | -1.9347% | 0.025 |

## Decision

Reject for paper trading. Separate linear experts do not create a stable automatic-switching edge.
This result also means the current Laya adaptation should not be promoted merely by replacing the
linear expert with a larger network: the underlying state/label representation has not passed the
non-neural feasibility gate.

Full metrics, hashes, and C++ tapes are in `results.json`.
