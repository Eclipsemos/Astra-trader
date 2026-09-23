"use client";

import { useEffect, useMemo, useState } from "react";
import styles from "./page.module.css";

type Status = {
  status: string; symbol: string; sequence: number; exchange_time_ns: number; receive_time_ns: number;
  best_bid_units: number; best_ask_units: number; position_units: number; equity_units: number;
  realized_pnl_units: number; unrealized_pnl_units: number; fees_units: number; orders: number; fills: number;
  model_action: "long" | "short" | "hold"; long_probability_ppm: number; short_probability_ppm: number;
  hold_probability_ppm: number; model_latency_ns: number; coalesced_model_requests: number;
  clock_offset_ns: number; adjusted_data_age_ns: number; trading_ready: boolean; audit_healthy: boolean; audit_dropped_events: number;
};

const API = process.env.NEXT_PUBLIC_ASTRA_API ?? "http://127.0.0.1:8765";
const money = (units: number) => (units / 100).toLocaleString(undefined, {minimumFractionDigits: 2, maximumFractionDigits: 2});
const price = (units: number) => (units / 100).toLocaleString(undefined, {minimumFractionDigits: 2, maximumFractionDigits: 2});
const pct = (ppm: number) => `${(ppm / 10_000).toFixed(2)}%`;
const age = (time: number) => time > 0 ? `${Math.round(Math.max(0, Date.now() * 1_000_000 - time) / 1_000_000)} ms` : "-";

function Metric({ label, value, tone }: { label: string; value: string; tone?: "positive" | "negative" }) {
  return <div className={styles.metric}><span>{label}</span><strong className={tone ? styles[tone] : ""}>{value}</strong></div>;
}

export default function Page() {
  const [status, setStatus] = useState<Status | null>(null);
  const [connected, setConnected] = useState(false);
  useEffect(() => {
    let alive = true;
    const load = async () => {
      try {
        const response = await fetch(`${API}/api/status`, { cache: "no-store" });
        if (!response.ok) throw new Error("status");
        const next = await response.json() as Status;
        if (alive) { setStatus(next); setConnected(true); }
      } catch { if (alive) setConnected(false); }
    };
    void load();
    const timer = window.setInterval(() => void load(), 1000);
    return () => { alive = false; window.clearInterval(timer); };
  }, []);

  const mid = useMemo(() => status ? (status.best_bid_units + status.best_ask_units) / 2 : 0, [status]);
  const actionTone = status?.model_action === "long" ? styles.positive : status?.model_action === "short" ? styles.negative : styles.neutral;
  return <main className={styles.shell}>
    <header className={styles.header}>
      <div><p className={styles.kicker}>ASTRA / PAPER CONTROL PLANE</p><h1 className={styles.title}>BTCUSDT decision trader</h1></div>
      <div className={styles.connection}><span className={connected ? styles.dotLive : styles.dotOff} />{connected ? "connected" : "waiting for backend"}<small>paper only</small></div>
    </header>
    <section className={styles.banner}>
      <div><span className={styles.label}>MARKET</span><strong>{status?.symbol ?? "BTCUSDT"}</strong><span className={styles.muted}> public Binance USD-M book ticker</span></div>
      <div className={styles.sequence}>event {status ? status.sequence.toLocaleString() : "-"}</div>
    </section>
    <section className={styles.metrics}>
      <Metric label="mid price" value={mid ? `$${price(mid)}` : "-"} />
      <Metric label="position" value={status ? `${status.position_units > 0 ? "+" : ""}${status.position_units}` : "-"} tone={status && status.position_units !== 0 ? status.position_units > 0 ? "positive" : "negative" : undefined} />
      <Metric label="paper equity" value={status ? `$${money(status.equity_units)}` : "-"} />
      <Metric label="realized pnl" value={status ? `$${money(status.realized_pnl_units)}` : "-"} tone={status && status.realized_pnl_units < 0 ? "negative" : "positive"} />
      <Metric label="fees" value={status ? `$${money(status.fees_units)}` : "-"} />
      <Metric label="fills / orders" value={status ? `${status.fills} / ${status.orders}` : "-"} />
    </section>
    <section className={styles.grid}>
      <article className={styles.panel}>
        <div className={styles.panelHead}><h2>Decision</h2><span className={actionTone}>{status?.model_action ?? "hold"}</span></div>
        <div className={styles.action}>{status?.model_action?.toUpperCase() ?? "HOLD"}</div>
        <div className={styles.probs}>
          <div><span>long</span><b>{status ? pct(status.long_probability_ppm) : "-"}</b><i style={{width: `${status ? status.long_probability_ppm / 10_000 : 0}%`}} className={styles.longBar} /></div>
          <div><span>short</span><b>{status ? pct(status.short_probability_ppm) : "-"}</b><i style={{width: `${status ? status.short_probability_ppm / 10_000 : 0}%`}} className={styles.shortBar} /></div>
          <div><span>hold</span><b>{status ? pct(status.hold_probability_ppm) : "-"}</b><i style={{width: `${status ? status.hold_probability_ppm / 10_000 : 0}%`}} className={styles.holdBar} /></div>
        </div>
        <div className={styles.detail}><span>model latency</span><b>{status ? `${(status.model_latency_ns / 1e6).toFixed(1)} ms` : "-"}</b></div>
        <div className={styles.detail}><span>model queue merges</span><b>{status?.coalesced_model_requests ?? "-"}</b></div>
      </article>
      <article className={styles.panel}>
        <div className={styles.panelHead}><h2>Top of book</h2><span className={styles.muted}>freshness {status ? age(status.receive_time_ns) : "-"}</span></div>
        <div className={styles.book}><div><span>bid</span><strong className={styles.positive}>{status ? `$${price(status.best_bid_units)}` : "-"}</strong></div><div><span>ask</span><strong className={styles.negative}>{status ? `$${price(status.best_ask_units)}` : "-"}</strong></div></div>
        <div className={styles.detail}><span>spread</span><b>{status ? `$${price(status.best_ask_units - status.best_bid_units)}` : "-"}</b></div>
        <div className={styles.detail}><span>unrealized pnl</span><b>{status ? `$${money(status.unrealized_pnl_units)}` : "-"}</b></div>
        <div className={styles.detail}><span>safety gate</span><b className={status?.trading_ready ? styles.positive : styles.negative}>{status?.trading_ready ? "ready" : "warming / hold"}</b></div>
        <div className={styles.detail}><span>host clock offset</span><b>{status ? `${(status.clock_offset_ns / 1e9).toFixed(2)} s` : "-"}</b></div>
        <div className={styles.detail}><span>adjusted feed age</span><b>{status ? `${(status.adjusted_data_age_ns / 1e6).toFixed(1)} ms` : "-"}</b></div>
        <div className={styles.detail}><span>audit</span><b>{status ? status.audit_healthy ? `healthy / ${status.audit_dropped_events} dropped` : "unavailable" : "-"}</b></div>
      </article>
    </section>
    <footer className={styles.footer}><span>Model suggestions are advisory; C++ risk and paper exchange are authoritative.</span><span>API {API}</span></footer>
  </main>;
}
