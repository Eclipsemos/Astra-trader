#!/usr/bin/env bash
set -euo pipefail

project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

bash "$project_root/scripts/run-laya.sh" >"${ASTRA_LAYA_LOG:-/tmp/astra-laya.log}" 2>&1 &
laya_pid=$!
bash "$project_root/scripts/run-paper.sh" >"${ASTRA_PAPER_LOG:-/tmp/astra-paper.log}" 2>&1 &
paper_pid=$!
bash "$project_root/scripts/run-dashboard.sh" >"${ASTRA_DASHBOARD_LOG:-/tmp/astra-dashboard.log}" 2>&1 &
dashboard_pid=$!

cleanup() {
  kill "$dashboard_pid" "$paper_pid" "$laya_pid" 2>/dev/null || true
  wait "$dashboard_pid" "$paper_pid" "$laya_pid" 2>/dev/null || true
}
trap cleanup INT TERM EXIT

echo "Astra paper stack running"
echo "dashboard: http://127.0.0.1:${ASTRA_DASHBOARD_PORT:-3000}"
echo "status:    http://127.0.0.1:8765/api/status"
echo "logs:      ${ASTRA_LAYA_LOG:-/tmp/astra-laya.log} ${ASTRA_PAPER_LOG:-/tmp/astra-paper.log} ${ASTRA_DASHBOARD_LOG:-/tmp/astra-dashboard.log}"
wait "$paper_pid"
