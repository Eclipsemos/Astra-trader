#!/usr/bin/env bash
set -euo pipefail

project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
if [[ ! -x "$project_root/.tools/bin/cmake" ]]; then
  "$project_root/scripts/bootstrap-tools.sh"
fi

export PATH="$project_root/.tools/bin:$PATH"
cmake --preset dev --fresh
cmake --build --preset dev
ctest --preset dev
