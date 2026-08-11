#!/usr/bin/env bash
# Downscale wide TXTR pages in data.win for Wii GX (see preprocess-wii-textures.py).
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
VENV="${ROOT}/tools/.venv-wii-tex"
PY="${VENV}/bin/python"
if [[ ! -x "${PY}" ]]; then
  python3 -m venv "${VENV}"
  "${VENV}/bin/pip" install -q pillow
fi
exec "${PY}" "${SCRIPT_DIR}/preprocess-wii-textures.py" "$@"
