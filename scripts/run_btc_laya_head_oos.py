#!/usr/bin/env python3
"""Fine-tune Laya's small choice scorer on BTC states and evaluate it out of sample.

The ModernBERT encoder and Laya decision transformer remain frozen.  Only the final scorer that
maps option-marker representations to the three BTC actions is updated.  This makes the first
adaptation experiment auditable and runnable on the current CPU-only host; it is not a claim that
full neural fine-tuning is economically justified.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import sys
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

import torch
import torch.nn.functional as F

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))
from run_btc_model_oos_report import (  # noqa: E402
    COSTS,
    SPLITS,
    build_samples,
    load_csv,
    prediction_metrics,
    simulate,
)


ACTION_NAMES = ("hold", "long", "short")
QUESTION = {
    "type": "choice",
    "instructions": (
        "For this BTCUSDT futures market state, select the next action. Trade only when the "
        "directional move is likely to exceed execution costs."
    ),
    "criteria": {
        "hold": "do not open or change exposure",
        "long": "open or maintain a long position",
        "short": "open or maintain a short position",
    },
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def state_text(row: dict[str, Any]) -> str:
    values = row["features"]
    fields = (
        "ret_1m",
        "ret_5m",
        "ret_15m",
        "ret_60m",
        "vol_60m",
        "flow_imbalance",
        "spread_bps",
        "hour_sin",
        "hour_cos",
    )
    return "BTCUSDT state: " + ", ".join(f"{name}={value:.8g}" for name, value in zip(fields, values))


def make_item(agent: Any, row: dict[str, Any]) -> dict[str, Any]:
    from laya.common import QTYPES, build_sequence

    internal = {"t": "choice", "ins": QUESTION["instructions"], "crit": QUESTION["criteria"]}
    ids, markers = build_sequence(
        agent.tok,
        state_text(row),
        internal,
        max_len=agent.cfg.get("max_len", 512),
        head_max_len=agent.cfg.get("head_max_len", 192),
    )
    return {
        "ids": ids,
        "markers": markers,
        "qtype": QTYPES["choice"],
        "label": int(row["label"]),
        "row_index": int(row["index"]),
    }


def batches(agent: Any, rows: list[dict[str, Any]], batch_size: int):
    from laya.common import collate_items

    items = [make_item(agent, row) for row in rows]
    for start in range(0, len(items), batch_size):
        chunk = items[start : start + batch_size]
        yield collate_items([chunk], agent.tok.pad_token_id), rows[start : start + batch_size]


def marker_logits(agent: Any, batch: dict[str, torch.Tensor], train: bool) -> torch.Tensor:
    model = agent.model
    device = agent.device
    input_ids = batch["input_ids"].to(device)
    attention_mask = batch["attention_mask"].to(device)
    marker_pos = batch["marker_pos"].to(device)
    marker_mask = batch["marker_mask"].to(device)
    qtype = batch["qtype"].to(device)
    with torch.no_grad():
        h = model.encoder(input_ids=input_ids, attention_mask=attention_mask).last_hidden_state
        h = h + model.type_emb(qtype)[:, None, :]
        if model.head is not None:
            pad = ~attention_mask.bool()
            for layer in model.head.layers:
                h = layer(h, src_key_padding_mask=pad)
        idx = marker_pos.clamp(min=0)[:, :, None].expand(-1, -1, h.size(-1))
        markers = torch.gather(h, 1, idx)
    if train:
        logits = model.scorer(markers).squeeze(-1).float()
    else:
        with torch.no_grad():
            logits = model.scorer(markers).squeeze(-1).float()
    return logits.masked_fill(~marker_mask, -1e4)


def collect_logits(agent: Any, rows: list[dict[str, Any]], batch_size: int) -> list[list[float]]:
    agent.model.eval()
    outputs: list[list[float]] = []
    for batch, _ in batches(agent, rows, batch_size):
        logits = marker_logits(agent, batch, train=False).cpu()
        outputs.extend(logits.tolist())
    return outputs


def train_scorer(agent: Any, rows: list[dict[str, Any]], batch_size: int, epochs: int, learning_rate: float) -> None:
    model = agent.model
    for parameter in model.parameters():
        parameter.requires_grad_(False)
    for parameter in model.scorer.parameters():
        parameter.requires_grad_(True)
    model.eval()
    model.scorer.train()
    counts = torch.bincount(torch.tensor([int(row["label"]) for row in rows]), minlength=3).float()
    class_weights = (len(rows) / (3.0 * counts.clamp_min(1))).clamp(0.5, 5.0).to(agent.device)
    optimizer = torch.optim.AdamW(model.scorer.parameters(), lr=learning_rate, weight_decay=0.001)
    for epoch in range(epochs):
        total_loss = 0.0
        total = 0
        for batch, batch_rows in batches(agent, rows, batch_size):
            optimizer.zero_grad(set_to_none=True)
            logits = marker_logits(agent, batch, train=True)
            labels = torch.tensor([int(row["label"]) for row in batch_rows], device=agent.device)
            loss = F.cross_entropy(logits, labels, weight=class_weights)
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.scorer.parameters(), 1.0)
            optimizer.step()
            total_loss += float(loss.detach().cpu()) * len(batch_rows)
            total += len(batch_rows)
        print(f"epoch={epoch + 1}/{epochs} loss={total_loss / max(1, total):.6f}", flush=True)


def probabilities(logits: list[list[float]], temperature: float) -> list[list[float]]:
    tensor = torch.tensor(logits, dtype=torch.float32) / temperature
    return torch.softmax(tensor, dim=-1).tolist()


def write_tape(path: Path, rows: list[dict[str, Any]], logits: list[list[float]], temperature: float, threshold: float) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    probs = probabilities(logits, temperature)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(("sequence", "action", "long_probability_ppm", "short_probability_ppm", "hold_probability_ppm"))
        for row, prediction in zip(rows, probs):
            direction = max((1, 2), key=lambda index: prediction[index])
            action = ACTION_NAMES[direction] if prediction[direction] >= threshold else "hold"
            writer.writerow(
                (
                    int(row["index"]) + 1,
                    action,
                    round(prediction[1] * 1_000_000),
                    round(prediction[2] * 1_000_000),
                    round(prediction[0] * 1_000_000),
                )
            )


def choose_temperature(rows: list[dict[str, Any]], logits: list[list[float]]) -> float:
    best = (float("inf"), 1.0)
    for temperature in (0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0):
        p = probabilities(logits, temperature)
        loss = -sum(torch.log(torch.tensor(max(1e-12, p[i][int(row["label"])]))).item() for i, row in enumerate(rows)) / len(rows)
        if loss < best[0]:
            best = (loss, temperature)
    return best[1]


def evaluate(rows: list[dict[str, Any]], logits: list[list[float]], temperature: float, threshold: float, data: tuple[Any, ...]) -> dict[str, Any]:
    predictions = probabilities(logits, temperature)
    summary, actions = prediction_metrics(rows, predictions, threshold)
    result = {"prediction": summary}
    for cost_name, costs in COSTS.items():
        result[cost_name] = simulate(rows, actions, data[1], data[2], data[5], *costs)
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=Path("data/replay/btc-2025-2026.csv"))
    parser.add_argument("--laya-root", type=Path, default=Path("/home/ldtdev/qt/mmAstra/laya"))
    parser.add_argument("--output-dir", type=Path, default=Path("reports/btc_laya_head_oos"))
    parser.add_argument("--stride", type=int, default=60)
    parser.add_argument("--horizon", type=int, default=60)
    parser.add_argument("--batch-size", type=int, default=16)
    parser.add_argument("--epochs", type=int, default=2)
    parser.add_argument("--device", default="cuda" if torch.cuda.is_available() else "cpu")
    parser.add_argument("--max-train", type=int, default=0, help="debug cap for train rows; never use for final evidence")
    parser.add_argument("--max-samples", type=int, default=0, help="debug cap applied to every split; never use for final evidence")
    args = parser.parse_args()
    if not args.input.exists():
        raise SystemExit(f"missing input: {args.input}")
    if not args.laya_root.exists():
        raise SystemExit(f"missing laya root: {args.laya_root}")
    sys.path.insert(0, str(args.laya_root))
    from laya import Agent

    data = load_csv(args.input)
    samples = build_samples(*data, args.stride, args.horizon, 0.0002)
    grouped = {name: [row for row in samples if row["split"] == name] for name in SPLITS}
    if args.max_samples:
        grouped = {name: rows[: args.max_samples] for name, rows in grouped.items()}
    train_rows = grouped["development_train"]
    if args.max_train:
        train_rows = train_rows[: args.max_train]
    if args.device.startswith("cuda") and not torch.cuda.is_available():
        raise SystemExit("CUDA was requested but torch.cuda.is_available() is false")
    agent = Agent("convaiinnovations/laya", device=args.device)
    base_logits = {name: collect_logits(agent, rows, args.batch_size) for name, rows in grouped.items()}
    train_scorer(agent, train_rows, args.batch_size, args.epochs, 2e-4)
    adapted_logits = {name: collect_logits(agent, rows, args.batch_size) for name, rows in grouped.items()}
    temperature = choose_temperature(grouped["development_validation"], adapted_logits["development_validation"])
    candidate_thresholds = []
    for threshold in (0.40, 0.45, 0.50, 0.55, 0.60, 0.65, 0.70):
        validation = evaluate(grouped["development_validation"], adapted_logits["development_validation"], temperature, threshold, data)
        candidate_thresholds.append((validation["base"]["return_ppm"], threshold, validation["base"]["fills"]))
    _, threshold, _ = max(candidate_thresholds, key=lambda item: (item[0], item[2], item[1]))
    adapted = {name: evaluate(rows, adapted_logits[name], temperature, threshold, data) for name, rows in grouped.items()}
    base_temperature = choose_temperature(grouped["development_validation"], base_logits["development_validation"])
    base = {name: evaluate(rows, base_logits[name], base_temperature, threshold, data) for name, rows in grouped.items()}
    args.output_dir.mkdir(parents=True, exist_ok=True)
    artifact_path = args.output_dir / "scorer.pt"
    torch.save({"scorer": agent.model.scorer.state_dict(), "temperature": temperature, "threshold": threshold}, artifact_path)
    tape_paths = {}
    for name, rows in grouped.items():
        tape_path = args.output_dir / f"{name}-adapted-tape.csv"
        write_tape(tape_path, rows, adapted_logits[name], temperature, threshold)
        tape_paths[name] = {"path": str(tape_path), "sha256": sha256(tape_path), "bytes": tape_path.stat().st_size}
    report = {
        "schema": "astra.report.btc-laya-head-oos.v1",
        "generated_at": datetime.now(UTC).isoformat(),
        "input": {"path": str(args.input), "sha256": sha256(args.input), "rows": len(data[0])},
        "protocol": {"stride": args.stride, "horizon": args.horizon, "epochs": args.epochs, "batch_size": args.batch_size, "device": str(agent.device), "gpu": torch.cuda.get_device_name(agent.device) if agent.device.type == "cuda" else None, "encoder_frozen": True, "trained_component": "model.scorer", "train_samples_used": len(train_rows), "samples": {name: len(rows) for name, rows in grouped.items()}},
        "calibration": {"temperature": temperature, "threshold": threshold, "threshold_candidates": [{"return_ppm": value, "threshold": candidate, "fills": fills} for value, candidate, fills in candidate_thresholds], "base_temperature": base_temperature},
        "generic_laya_frozen": base,
        "btc_laya_head_adapted": adapted,
        "artifact": {"path": str(artifact_path), "sha256": sha256(artifact_path), "bytes": artifact_path.stat().st_size},
        "decision_tapes": tape_paths,
        "decision": {"paper_approved": False, "reason": "Head-only adaptation is an experiment; 2026 OOS must clear the existing paper gates before promotion."},
    }
    (args.output_dir / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    (args.output_dir / "README.md").write_text(markdown(report), encoding="utf-8")
    print(json.dumps({"output": str(args.output_dir), "holdout_base": adapted["holdout_2026"]["base"], "holdout_stress": adapted["holdout_2026"]["stress"]}, indent=2))


def markdown(report: dict[str, Any]) -> str:
    lines = [
        "# BTC-adapted Laya head OOS validation",
        "",
        "This experiment freezes Laya's ModernBERT encoder and decision transformer, and trains only",
        "the final choice scorer on causal BTC states. It compares the frozen generic checkpoint with",
        "the adapted scorer on the same chronological samples and paper execution simulator.",
        "",
        "## Results",
        "",
        "| Split | Model | Base return | Base PF | Base fills | Stress return | Stress PF | Stress fills |",
        "|---|---|---:|---:|---:|---:|---:|---:|",
    ]
    for split in SPLITS:
        for name, key in (("generic Laya", "generic_laya_frozen"), ("BTC head-adapted", "btc_laya_head_adapted")):
            base = report[key][split]["base"]
            stress = report[key][split]["stress"]
            lines.append(
                f"| {split} | {name} | {base['return_ppm'] / 10_000:.4f}% | {base['profit_factor']} | {base['fills']} | "
                f"{stress['return_ppm'] / 10_000:.4f}% | {stress['profit_factor']} | {stress['fills']} |"
            )
    lines += [
        "",
        f"Validation temperature: `{report['calibration']['temperature']}`; action threshold: `{report['calibration']['threshold']}`.",
        "",
        "## Protocol",
        "",
        f"- Input: `{report['input']['path']}`; `{report['input']['rows']:,}` one-minute events.",
        f"- Samples: stride `{report['protocol']['stride']}` minutes, forward horizon `{report['protocol']['horizon']}` minutes.",
        f"- Training device: `{report['protocol']['device']}` (`{report['protocol']['gpu'] or 'CPU'}`).",
        f"- Train/validation/holdout samples: `{report['protocol']['samples']}`.",
        "- Training uses only development train; temperature and action threshold use development validation; 2026 is untouched.",
        "- The scorer artifact is written to local research output and is identified by SHA-256 in `results.json`; it is not loaded by paper mode.",
        "- Each split also emits a decision tape consumed by C++ `astra_replay --model tape`; the Python simulator is only a pre-check.",
        "",
        "## Decision",
        "",
        "This is not a paper approval. The adapted checkpoint must pass rolling OOS, cost stress, and uncertainty gates before the C++ gateway can load it.",
        "The current run uses the existing minute cache derived from aggregate trades; the raw ZIP audit is recorded separately before the next tick-level dataset build.",
        "",
        "Generated by `scripts/run_btc_laya_head_oos.py`.",
        "",
    ]
    return "\n".join(lines)


if __name__ == "__main__":
    main()
