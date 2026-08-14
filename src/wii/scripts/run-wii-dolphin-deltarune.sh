#!/usr/bin/env bash
# Stage one Deltarune chapter as games/dr on the shared Dolphin SD tree.
# Undertale lives in games/ut — this script does not delete it.
#
# Usage:
#   src/wii/scripts/run-wii-dolphin-deltarune.sh 1
#   src/wii/scripts/run-wii-dolphin-deltarune.sh 1 --bake
#   src/wii/scripts/run-wii-dolphin-deltarune.sh 1 --no-launch
#
# Env:
#   DELTARUNE_DIR   Windows depot (default: ~/Downloads/deltarune-win)
#   DELTARUNE_SAVE  override path to filechN_0
#   DOLPHIN_USER    default: <repo>/.dolphin-user  (shared with Undertale)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
APP_DIR="${REPO_ROOT}/build-wii/apps/butterscotch"
USERDIR="${DOLPHIN_USER:-${REPO_ROOT}/.dolphin-user}"
SYNC="${USERDIR}/Load/WiiSDSync"
APP_SYNC="${SYNC}/apps/butterscotch"
GAME_SYNC="${APP_SYNC}/games/dr"
DELTARUNE_DIR="${DELTARUNE_DIR:-${HOME}/Downloads/deltarune-win}"
VENV="${REPO_ROOT}/tools/.venv-wii-tex"
DOLPHIN_BIN="/Applications/Dolphin.app/Contents/MacOS/Dolphin"

CHAPTER=""
BAKE=0
NO_LAUNCH=0
RESEED_SAVE=0

usage() {
  cat <<EOF
Usage: $0 <chapter 1-5> [--bake] [--reseed-save] [--no-launch]
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    --bake) BAKE=1 ;;
    --no-launch) NO_LAUNCH=1 ;;
    --reseed-save) RESEED_SAVE=1 ;;
    [1-5]) CHAPTER="$1" ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
  shift
done

if [[ -z "${CHAPTER}" ]]; then
  echo "Need a chapter number (1-5)." >&2
  usage >&2
  exit 1
fi

CH_DIR="${DELTARUNE_DIR}/chapter${CHAPTER}_windows"
WTL="${APP_DIR}/data.win.deltarune-ch${CHAPTER}.wtl1"
SRC_DATA="${CH_DIR}/data.win"
SAVE_SRC="${DELTARUNE_SAVE:-${HOME}/Downloads/filech${CHAPTER}_0}"
DOL="${APP_DIR}/boot.dol"
OLD_SAVE="${REPO_ROOT}/.dolphin-user-deltarune/Load/WiiSDSync/apps/butterscotch/saves/filech${CHAPTER}_0"

if [[ ! -f "${DOL}" ]]; then
  echo "Missing ${DOL}. Build first with src/wii/scripts/build-wii-docker.sh" >&2
  exit 1
fi
if [[ ! -d "${CH_DIR}" || ! -f "${SRC_DATA}" ]]; then
  echo "Missing chapter ${CHAPTER} at ${CH_DIR}" >&2
  exit 1
fi
if [[ ! -x "${DOLPHIN_BIN}" && "${NO_LAUNCH}" -eq 0 ]]; then
  echo "Missing Dolphin at ${DOLPHIN_BIN}" >&2
  exit 1
fi

if [[ -f "${SCRIPT_DIR}/validate-wii-dol.py" ]]; then
  python3 "${SCRIPT_DIR}/validate-wii-dol.py" "${DOL}"
fi

if [[ "${BAKE}" -eq 1 || ! -f "${WTL}" ]]; then
  echo "Baking WTL2: ${SRC_DATA} -> ${WTL}"
  python3 "${SCRIPT_DIR}/bs-convert.py" dr -i "${SRC_DATA}" -o "${WTL}"
fi

mkdir -p "${GAME_SYNC}/saves" "${GAME_SYNC}/mus" "${GAME_SYNC}/lang" "${APP_SYNC}/saves" "${USERDIR}/Config"

cp -f "${DOL}" "${APP_SYNC}/boot.dol"
if [[ -f "${APP_DIR}/meta.xml" ]]; then cp -f "${APP_DIR}/meta.xml" "${APP_SYNC}/meta.xml"; fi
if [[ -f "${APP_DIR}/icon.png" ]]; then cp -f "${APP_DIR}/icon.png" "${APP_SYNC}/icon.png"; fi
cp -f "${WTL}" "${GAME_SYNC}/data.win"
printf '%s\n' "{\"title\":\"Deltarune Chapter ${CHAPTER}\",\"id\":\"dr\"}" > "${GAME_SYNC}/game.json"

shopt -s nullglob
for f in "${CH_DIR}"/*.ogg "${CH_DIR}/audiogroup1.dat" "${CH_DIR}/options.ini"; do
  [[ -f "${f}" ]] || continue
  cp -f "${f}" "${GAME_SYNC}/"
done
shopt -u nullglob
if [[ -d "${CH_DIR}/lang" ]]; then
  rsync -a "${CH_DIR}/lang/" "${GAME_SYNC}/lang/"
fi
if [[ -d "${DELTARUNE_DIR}/mus" ]]; then
  rsync -a "${DELTARUNE_DIR}/mus/" "${GAME_SYNC}/mus/"
fi

shopt -s nullglob
for f in "${GAME_SYNC}/saves"/filech*_0; do
  [[ "$(basename "${f}")" == "filech${CHAPTER}_0" ]] && continue
  rm -f "${f}"
done
shopt -u nullglob

SAVE_DST="${GAME_SYNC}/saves/filech${CHAPTER}_0"
if [[ "${RESEED_SAVE}" -eq 1 || ! -f "${SAVE_DST}" ]]; then
  if [[ -f "${SAVE_SRC}" ]]; then
    cp -f "${SAVE_SRC}" "${SAVE_DST}"
    echo "Seeded save: ${SAVE_DST}"
  elif [[ -f "${OLD_SAVE}" ]]; then
    cp -f "${OLD_SAVE}" "${SAVE_DST}"
    echo "Migrated save from .dolphin-user-deltarune: ${SAVE_DST}"
  else
    echo "No filech${CHAPTER}_0 at ${SAVE_SRC} (title screen / empty slot)."
  fi
else
  echo "Keeping existing SD save: ${SAVE_DST} (pass --reseed-save to replace)"
fi

cat > "${USERDIR}/Config/Dolphin.ini" << INI
[Core]
GFXBackend = Metal
WiiSDCard = True
WiiSDCardEnableFolderSync = True
WiiSDCardAllowWrites = True
WiiSDCardSyncFolder = ${SYNC}
INI

python3 - <<PY
from pathlib import Path
p = Path("${GAME_SYNC}/data.win")
d = p.read_bytes()
print(f"data.win {p.stat().st_size} WTL2={d.count(b'WTL2')} WTL1={d.count(b'WTL1')} 2zoq={d.count(b'2zoq')}")
PY

echo "Chapter ${CHAPTER} staged at ${GAME_SYNC}"
echo "Dolphin user dir: ${USERDIR} (shared SD — Undertale is games/ut if staged)"

if [[ "${NO_LAUNCH}" -eq 1 ]]; then
  exit 0
fi

pkill -x Dolphin 2>/dev/null || true
sleep 1

echo "Launching Dolphin with: ${APP_SYNC}/boot.dol"
exec "${DOLPHIN_BIN}" \
  -u "${USERDIR}" \
  -v Metal \
  -e "${APP_SYNC}/boot.dol"
