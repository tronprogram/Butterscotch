#!/usr/bin/env bash
# Launch mainline Dolphin with Butterscotch Wii build + SD folder sync.
# Interactive use only — Felk/MCP scripting is Windows-only.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
APP_DIR="${REPO_ROOT}/build-wii/apps/butterscotch"
USERDIR="${DOLPHIN_USER:-${REPO_ROOT}/.dolphin-user}"
SYNC="${USERDIR}/Load/WiiSDSync"

DOL="${1:-}"
if [[ -z "${DOL}" ]]; then
  if [[ -f "${APP_DIR}/boot.dol" ]]; then
    DOL="${APP_DIR}/boot.dol"
  elif [[ -f "${REPO_ROOT}/build-wii/butterscotch.dol" ]]; then
    DOL="${REPO_ROOT}/build-wii/butterscotch.dol"
  else
    echo "No .dol found. Build first with scripts/build-wii-docker.sh" >&2
    exit 1
  fi
fi
DOL="$(cd "$(dirname "${DOL}")" && pwd)/$(basename "${DOL}")"

if [[ ! -f "${APP_DIR}/data.win" ]]; then
  echo "Missing ${APP_DIR}/data.win (place Undertale data.win + oggs beside boot.dol)" >&2
  exit 1
fi

if [[ -f "${REPO_ROOT}/scripts/validate-wii-dol.py" ]]; then
  python3 "${REPO_ROOT}/scripts/validate-wii-dol.py" "${DOL}"
fi

mkdir -p "${SYNC}/apps/butterscotch" "${USERDIR}/Config"
# Never clobber live SD saves with anything under build-wii/.../saves/
# (a debug LV20 barrier drop-in was reseeding on every launch).
rsync -a --delete --exclude 'saves/' "${APP_DIR}/" "${SYNC}/apps/butterscotch/"
mkdir -p "${SYNC}/apps/butterscotch/saves"

# Folder-sync SD so sd:/apps/butterscotch/data.win resolves.
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
echo "SD sync folder: ${SYNC}"
exec /Applications/Dolphin.app/Contents/MacOS/Dolphin \
  -u "${USERDIR}" \
  -v Metal \
  -e "${DOL}"
