#!/usr/bin/env bash
set -euo pipefail

project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
laya_root=${LAYA_ROOT:-"$project_root/../laya"}
laya_executable="$laya_root/.venv/bin/laya-serve"

if [[ ! -x "$laya_executable" ]]; then
  echo "Laya is not installed at $laya_executable" >&2
  echo "Create its virtual environment and install /home/ldtdev/qt/mmAstra/laya[serve]." >&2
  exit 1
fi

export LAYA_HOST=127.0.0.1
export LAYA_PORT=${LAYA_PORT:-8000}
export LAYA_DEVICE=${LAYA_DEVICE:-cpu}
export LAYA_PRELOAD=1
export LAYA_MODELS=${LAYA_MODELS:-english}
export LAYA_THREADS=${LAYA_THREADS:-12}

exec "$laya_executable"
