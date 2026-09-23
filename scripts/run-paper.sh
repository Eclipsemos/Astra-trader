#!/usr/bin/env bash
set -euo pipefail

project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
if [[ ! -x "$project_root/build/release/astra_trader" ]]; then
  if [[ ! -x "$project_root/.tools/bin/cmake" ]]; then
    bash "$project_root/scripts/bootstrap-tools.sh"
  fi
  export PATH="$project_root/.tools/bin:$PATH"
  cmake --preset release --fresh
  cmake --build --preset release
  ctest --preset release
fi

cd "$project_root"
exec "$project_root/build/release/astra_trader" --live "$@"
