#!/usr/bin/env bash
# Build a Wii-friendly data.win:
#  1) ÷2 any width>1024 pages except dialog/battle atlases 23/25
#  2) repack 23/25 at 1:1 into tall 1024×N atlases stored as WTL1 (tiled PNGs)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VENV="${ROOT}/tools/.venv-wii-tex"
PY="${VENV}/bin/python"
if [[ ! -x "${PY}" ]]; then
  python3 -m venv "${VENV}"
  "${VENV}/bin/pip" install -q pillow
fi

ORIG="${1:-${ROOT}/build-wii/apps/butterscotch/data.win.orig}"
OUT="${2:-${ROOT}/build-wii/apps/butterscotch/data.win}"
TMP="$(mktemp)"
trap 'rm -f "${TMP}"' EXIT

if [[ ! -f "${ORIG}" ]]; then
  echo "missing pristine data.win: ${ORIG}" >&2
  exit 1
fi

"${ROOT}/scripts/preprocess-wii-textures.sh" "${ORIG}" -o "${TMP}" --exclude "23,25"
"${PY}" "${ROOT}/scripts/repack-wii-face-atlases.py" "${ORIG}" --base "${TMP}" -o "${OUT}"
cp -f "${OUT}" "${ROOT}/.dolphin-user/Load/WiiSDSync/apps/butterscotch/data.win"
echo "synced Dolphin SD copy"
