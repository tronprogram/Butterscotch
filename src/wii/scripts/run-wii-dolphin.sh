#!/usr/bin/env bash
# Launch Dolphin with Undertale staged as games/ut (does not wipe games/dr).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
APP_DIR="${REPO_ROOT}/build-wii/apps/butterscotch"
USERDIR="${DOLPHIN_USER:-${REPO_ROOT}/.dolphin-user}"
SYNC="${USERDIR}/Load/WiiSDSync"
APP_SYNC="${SYNC}/apps/butterscotch"
GAME_SYNC="${APP_SYNC}/games/ut"

DOL="${1:-}"
if [[ -z "${DOL}" ]]; then
  if [[ -f "${APP_DIR}/boot.dol" ]]; then
    DOL="${APP_DIR}/boot.dol"
  elif [[ -f "${REPO_ROOT}/build-wii/butterscotch.dol" ]]; then
    DOL="${REPO_ROOT}/build-wii/butterscotch.dol"
  else
    echo "No .dol found. Build first with src/wii/scripts/build-wii-docker.sh" >&2
    exit 1
  fi
fi
DOL="$(cd "$(dirname "${DOL}")" && pwd)/$(basename "${DOL}")"

UT_WIN="${APP_DIR}/data.win"
if [[ ! -f "${UT_WIN}" ]]; then
  echo "Missing ${UT_WIN} (convert Undertale with src/wii/scripts/bs-convert.py ut …)" >&2
  exit 1
fi

if [[ -f "${SCRIPT_DIR}/validate-wii-dol.py" ]]; then
  python3 "${SCRIPT_DIR}/validate-wii-dol.py" "${DOL}"
fi

mkdir -p "${GAME_SYNC}/saves" "${APP_SYNC}/saves" "${USERDIR}/Config"
cp -f "${DOL}" "${APP_SYNC}/boot.dol"
if [[ -f "${APP_DIR}/meta.xml" ]]; then cp -f "${APP_DIR}/meta.xml" "${APP_SYNC}/meta.xml"; fi
if [[ -f "${APP_DIR}/icon.png" ]]; then cp -f "${APP_DIR}/icon.png" "${APP_SYNC}/icon.png"; fi
cp -f "${UT_WIN}" "${GAME_SYNC}/data.win"
shopt -s nullglob
for f in "${APP_DIR}"/*.ogg "${APP_DIR}/CONFIG.JSN"; do
  [[ -f "${f}" ]] || continue
  cp -f "${f}" "${GAME_SYNC}/"
done
shopt -u nullglob
printf '%s\n' '{"title":"Undertale","id":"ut"}' > "${GAME_SYNC}/game.json"

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

echo "Launching Dolphin with: ${DOL}"
echo "Game folder: ${GAME_SYNC}"
exec /Applications/Dolphin.app/Contents/MacOS/Dolphin \
  -u "${USERDIR}" \
  -v Metal \
  -e "${APP_SYNC}/boot.dol"
