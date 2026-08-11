#!/usr/bin/env bash
set -euo pipefail

# Source DevkitPro environment (MSYS2 or Linux)
if [[ -f /etc/profile.d/devkit-env.sh ]]; then
  # shellcheck disable=SC1091
  source /etc/profile.d/devkit-env.sh
fi

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
# On Windows/MSYS2 the SDK often lives under /c/msys64/opt/devkitpro
if [[ ! -d "${DEVKITPRO}" && -d /c/msys64/opt/devkitpro ]]; then
  export DEVKITPRO=/c/msys64/opt/devkitpro
fi
# Normalize Windows-style DEVKITPRO for MSYS bash
if [[ "${DEVKITPRO}" =~ ^[A-Za-z]: ]]; then
  drive="${DEVKITPRO:0:1}"
  rest="${DEVKITPRO:2}"
  export DEVKITPRO="/${drive,,}${rest//\\//}"
fi
export DEVKITPPC="${DEVKITPPC:-${DEVKITPRO}/devkitPPC}"
# Normalize DEVKITPPC similarly if needed
if [[ "${DEVKITPPC}" =~ ^[A-Za-z]: ]]; then
  drive="${DEVKITPPC:0:1}"
  rest="${DEVKITPPC:2}"
  export DEVKITPPC="/${drive,,}${rest//\\//}"
fi
export PATH="${DEVKITPPC}/bin:${DEVKITPRO}/tools/bin:${PATH}"

if ! command -v powerpc-eabi-gcc >/dev/null 2>&1; then
  echo "powerpc-eabi-gcc not found. DEVKITPRO=${DEVKITPRO}" >&2
  exit 1
fi
if ! command -v cmake >/dev/null 2>&1; then
  echo "cmake not found in MSYS2. Install with: pacman -S cmake" >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

cmake \
    -S "${REPO_ROOT}" \
    -B "${REPO_ROOT}/build-wii" \
    -G "Unix Makefiles" \
    -DPLATFORM=wii \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="${DEVKITPRO}/cmake/Wii.cmake"

cmake --build "${REPO_ROOT}/build-wii" -j"$(nproc 2>/dev/null || echo 4)"

echo ""
echo "Build complete."

DOL_BOOT="${REPO_ROOT}/build-wii/apps/butterscotch/boot.dol"
DOL_MAIN="${REPO_ROOT}/build-wii/butterscotch.dol"

if [ -f "${DOL_BOOT}" ]; then
    echo "HBC package : ${DOL_BOOT}"
fi
if [ -f "${DOL_MAIN}" ]; then
    echo "Raw DOL     : ${DOL_MAIN}"
fi
