#!/usr/bin/env bash
# Install / refresh Butterscotch Wii homebrew host dependencies (devkitPro).
# Intended for MSYS2 on Windows or any pacman host with dkp repos configured.
#
# Required for native GX work:
#   - wii-dev  (devkitPPC + libogc + wiiload + examples + cmake helpers)
#   - make
#
# Cloudflare sometimes 403s default pacman downloaders; use the wget UA below.

set -euo pipefail

PACMAN_CONF="${PACMAN_CONF:-/etc/pacman.conf}"

if ! grep -q '^\[dkp-libs\]' "$PACMAN_CONF" 2>/dev/null; then
  cat <<'EOF' >>"$PACMAN_CONF"

# devkitPro (Wii / GameCube / etc. homebrew toolchains)
[dkp-libs]
Server = https://pkg.devkitpro.org/packages

[dkp-windows]
Server = https://pkg.devkitpro.org/packages/windows/$arch/
EOF
  echo "Added dkp repos to $PACMAN_CONF"
fi

if ! grep -q '^XferCommand = /usr/bin/wget -U "dkp-pacman"' "$PACMAN_CONF" 2>/dev/null; then
  if grep -q '^#XferCommand = /usr/bin/wget --passive-ftp -c -O %o %u' "$PACMAN_CONF"; then
    sed -i 's|^#XferCommand = /usr/bin/wget --passive-ftp -c -O %o %u|XferCommand = /usr/bin/wget -U "dkp-pacman" --passive-ftp -c -O %o %u|' "$PACMAN_CONF"
  else
    sed -i '/^\[options\]/a XferCommand = /usr/bin/wget -U "dkp-pacman" --passive-ftp -c -O %o %u' "$PACMAN_CONF"
  fi
  echo "Enabled dkp-pacman wget XferCommand in $PACMAN_CONF"
fi

if ! pacman -Q devkitpro-keyring >/dev/null 2>&1; then
  tmp="$(mktemp)"
  wget -U "dkp-pacman" -O "$tmp" https://pkg.devkitpro.org/devkitpro-keyring.pkg.tar.xz
  pacman -U --noconfirm "$tmp"
  rm -f "$tmp"
fi

pacman -Sy --noconfirm
pacman -S --noconfirm --needed \
  make \
  cmake \
  devkit-env \
  dkp-toolchain-vars \
  wii-dev

# shellcheck disable=SC1091
source /etc/profile.d/devkit-env.sh
export PATH="${DEVKITPPC}/bin:${DEVKITPRO}/tools/bin:${PATH}"

echo "DEVKITPRO=$DEVKITPRO"
echo "DEVKITPPC=$DEVKITPPC"
powerpc-eabi-gcc --version | head -1
test -f "$DEVKITPRO/libogc/include/ogc/gx.h"
test -f "$DEVKITPRO/cmake/Wii.cmake"
echo "Wii deps OK (native GX via libogc)."
