#!/usr/bin/env bash
set -euo pipefail
project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
frontend_root="$project_root/frontend"
cd "$frontend_root"
if command -v bun >/dev/null 2>&1; then
  if [[ ! -d node_modules ]]; then bun install; fi
  if [[ ! -f .next/BUILD_ID ]]; then bun run build; fi
  exec bun run start --hostname 127.0.0.1 --port "${ASTRA_DASHBOARD_PORT:-3000}"
fi
if [[ ! -d node_modules ]]; then npm install; fi
if [[ ! -f .next/BUILD_ID ]]; then npm run build; fi
exec npm run start -- --hostname 127.0.0.1 --port "${ASTRA_DASHBOARD_PORT:-3000}"
