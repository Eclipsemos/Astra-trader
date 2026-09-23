#!/usr/bin/env bash
set -euo pipefail

project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
tools_root="$project_root/.tools"

if [[ ! -x "$tools_root/bin/python" ]]; then
  python3 -m venv "$tools_root"
fi

"$tools_root/bin/python" -m pip install --disable-pip-version-check \
  --requirement "$project_root/tools-requirements.txt"
"$tools_root/bin/cmake" --version | head -1
"$tools_root/bin/ninja" --version
