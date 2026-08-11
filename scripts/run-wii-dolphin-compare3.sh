#!/usr/bin/env bash
# Launch up to three Dolphin instances for pacing A/B/C compare.
# Default: open UI only (do NOT auto-boot) — booting three Wii Undertales at once
# OOMs a 16GB Mac. Start games manually, one at a time, after all windows are up.
#
# Variants (DOLs in build-wii-compare/<name>/boot.dol):
#   main    — git main baseline
#   30draw  — 30Hz step + draw/present on step only
#   60draw  — 30Hz step + draw/present every VI
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
APP_DIR="${REPO_ROOT}/build-wii/apps/butterscotch"
COMPARE_DIR="${REPO_ROOT}/build-wii-compare"
DOLPHIN_BIN="/Applications/Dolphin.app/Contents/MacOS/Dolphin"

AUTO_BOOT=0
if [[ "${1:-}" == "--boot" ]]; then
  AUTO_BOOT=1
  shift
fi

VARIANTS=("$@")
if [[ ${#VARIANTS[@]} -eq 0 ]]; then
  VARIANTS=(main 30draw 60draw)
fi

if [[ ! -f "${APP_DIR}/data.win" ]]; then
  echo "Missing ${APP_DIR}/data.win" >&2
  exit 1
fi
if [[ ! -x "${DOLPHIN_BIN}" ]]; then
  echo "Missing Dolphin at ${DOLPHIN_BIN}" >&2
  exit 1
fi

# Soft GFX / CPU knobs to cut host RAM per instance.
CONFIG_FLAGS=(
  -C "Dolphin.Display.Fullscreen=False"
  -C "Graphics.Settings.InternalResolution=1"
  -C "Graphics.Settings.MSAA=0"
  -C "Graphics.Settings.SSAA=False"
  -C "Graphics.Settings.WaitForShadersBeforeStarting=False"
  -C "Core.CPUThread=True"
  -C "Core.EmulationSpeed=1"
)

prepare_user() {
  local name="$1"
  local dol="${COMPARE_DIR}/${name}/boot.dol"
  local userdir="${REPO_ROOT}/.dolphin-user-${name}"
  local sync="${userdir}/Load/WiiSDSync"
  local app_sync="${sync}/apps/butterscotch"

  if [[ ! -f "${dol}" ]]; then
    echo "Missing DOL for ${name}: ${dol}" >&2
    exit 1
  fi

  mkdir -p "${app_sync}" "${userdir}/Config" "${app_sync}/saves"
  # Full real copy — Dolphin Wii SD folder-sync does not reliably follow
  # host symlinks that point outside the sync tree (manifested as nearly
  # empty rooms: e.g. only the flower bed, TXTR 4/26).
  rsync -a --delete --exclude 'saves/' "${APP_DIR}/" "${app_sync}/"
  cp -f "${dol}" "${app_sync}/boot.dol"

  cat > "${userdir}/Config/Dolphin.ini" << INI
[Core]
GFXBackend = Metal
WiiSDCard = True
WiiSDCardEnableFolderSync = True
WiiSDCardAllowWrites = True
WiiSDCardSyncFolder = ${sync}
INI

  echo "${userdir}|${dol}"
}

echo "Preparing ${#VARIANTS[@]} Dolphin user dirs..."
declare -a LAUNCH_USER LAUNCH_DOL
for name in "${VARIANTS[@]}"; do
  IFS='|' read -r ud dol < <(prepare_user "${name}")
  LAUNCH_USER+=("${ud}")
  LAUNCH_DOL+=("${dol}")
  echo "  ${name}: ${dol}"
done

# Do not pkill other Dolphins — user may have one open for work.
echo
if [[ "${AUTO_BOOT}" -eq 1 ]]; then
  echo "AUTO_BOOT=1: starting emulation immediately (heavy — may OOM on 16GB)."
else
  echo "Opening Dolphin UIs only. In each window: Open ${COMPARE_DIR}/<variant>/boot.dol"
  echo "Boot ONE game at a time; wait until stable before starting the next."
  echo "Pass --boot to auto-exec (not recommended for three-at-once on 16GB)."
fi
echo

i=0
for name in "${VARIANTS[@]}"; do
  userdir="${LAUNCH_USER[$i]}"
  dol="${LAUNCH_DOL[$i]}"
  # open -n forces a new macOS app instance (bundle id otherwise can coalesce).
  if [[ "${AUTO_BOOT}" -eq 1 ]]; then
    open -n -a Dolphin --args -u "${userdir}" -v Metal "${CONFIG_FLAGS[@]}" -e "${dol}"
  else
    open -n -a Dolphin --args -u "${userdir}" -v Metal "${CONFIG_FLAGS[@]}"
  fi
  echo "launched UI: ${name} (user ${userdir})"
  i=$((i + 1))
  # Stagger so Metal/device init does not spike together.
  sleep 3
done

echo
echo "Done. Labels: main | 30draw | 60draw  (check which DOL you open in each)."
