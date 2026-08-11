#!/usr/bin/env bash
# Launch the standalone Wii GX demo in Dolphin (no data.win required).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
OUT_DIR="${REPO_ROOT}/build-wii-gx-demo"
USERDIR="${DOLPHIN_USER:-${REPO_ROOT}/.dolphin-user-gx-demo}"
SYNC="${USERDIR}/Load/WiiSDSync"

DOL="${1:-${OUT_DIR}/boot.dol}"
if [[ ! -f "${DOL}" ]]; then
  echo "Missing ${DOL}. Build with ./src/wii/scripts/build-wii-gx-demo-docker.sh" >&2
  exit 1
fi
DOL="$(cd "$(dirname "${DOL}")" && pwd)/$(basename "${DOL}")"

if [[ -f "${SCRIPT_DIR}/validate-wii-dol.py" ]]; then
  if ! python3 "${SCRIPT_DIR}/validate-wii-dol.py" "${DOL}"; then
    echo "Padding DOL for Dolphin IPL alignment..." >&2
    python3 "${SCRIPT_DIR}/pad-wii-dol.py" "${DOL}"
    python3 "${SCRIPT_DIR}/validate-wii-dol.py" "${DOL}"
  fi
fi

mkdir -p "${SYNC}/apps/wii-gx-demo" "${USERDIR}/Config"
if [[ -d "${OUT_DIR}/apps/wii-gx-demo" ]]; then
  rsync -a --delete "${OUT_DIR}/apps/wii-gx-demo/" "${SYNC}/apps/wii-gx-demo/"
else
  cp -f "${DOL}" "${SYNC}/apps/wii-gx-demo/boot.dol"
fi

cat > "${USERDIR}/Config/Dolphin.ini" << INI
[Core]
GFXBackend = Metal
WiiSDCard = True
WiiSDCardEnableFolderSync = True
WiiSDCardAllowWrites = True
WiiSDCardSyncFolder = ${SYNC}
INI

pkill -x Dolphin 2>/dev/null || true
sleep 1

echo "Launching GX demo: ${DOL}"
echo "SD sync folder: ${SYNC}"
echo "HUD: green=FPS bar, red=draw-ms, blue=sprite count"
echo "Controls: D-pad L/R sprites, U/D scroll, A tall/wide, B tiled, 1 cross-slice, HOME quit"
exec /Applications/Dolphin.app/Contents/MacOS/Dolphin \
  -u "${USERDIR}" \
  -v Metal \
  -e "${DOL}"
