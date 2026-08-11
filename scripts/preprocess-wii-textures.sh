#!/usr/bin/env bash
# Downscale wide TXTR pages in data.win for Wii GX (see scripts/preprocess-wii-textures.py).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VENV="${ROOT}/tools/.venv-wii-tex"
PY="${VENV}/bin/python"
if [[ ! -x "${PY}" ]]; then
  python3 -m venv "${VENV}"
  "${VENV}/bin/pip" install -q pillow
fi
exec "${PY}" "${ROOT}/scripts/preprocess-wii-textures.py" "$@"
