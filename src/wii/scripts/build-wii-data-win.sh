#!/usr/bin/env bash
# Build a Wii-friendly data.win:
#  1) ÷2 any leftover width>1024 pages (none expected once WTL1 covers all wide ones)
#  2) repack every originally-wide page at 1:1 into tall 1024×N WTL1 atlases
#     (pages 1/11/14/20/23/25) — GX loads one ≤1024-tall tile at a time
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
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

# All pristine width>1024 pages — keep in sync with repack-wii-face-atlases.py --pages
WTL1_PAGES="1,11,14,20,23,25"

if [[ ! -f "${ORIG}" ]]; then
  echo "missing pristine data.win: ${ORIG}" >&2
  exit 1
fi

"${SCRIPT_DIR}/preprocess-wii-textures.sh" "${ORIG}" -o "${TMP}" --exclude "${WTL1_PAGES}"
"${PY}" "${SCRIPT_DIR}/repack-wii-face-atlases.py" "${ORIG}" --base "${TMP}" --pages "${WTL1_PAGES}" -o "${OUT}"
cp -f "${OUT}" "${ROOT}/.dolphin-user/Load/WiiSDSync/apps/butterscotch/data.win"
echo "synced Dolphin SD copy"
