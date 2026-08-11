#!/usr/bin/env bash
# Build tools/wii-gx-demo inside the official multi-arch devkitPro image.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
DEMO_DIR="${REPO_ROOT}/tools/wii-gx-demo"
OUT_DIR="${REPO_ROOT}/build-wii-gx-demo"

IMAGE="${DKP_IMAGE:-devkitpro/devkitppc:latest}"
PLATFORM="${DKP_PLATFORM:-}"
if [[ -z "${PLATFORM}" ]]; then
  case "$(uname -m)" in
    arm64|aarch64) PLATFORM=linux/arm64 ;;
    x86_64|amd64)  PLATFORM=linux/amd64 ;;
    *)
      echo "Unsupported host arch: $(uname -m). Set DKP_PLATFORM=linux/arm64|linux/amd64." >&2
      exit 1
      ;;
  esac
fi

if ! command -v docker >/dev/null 2>&1; then
  echo "docker not found." >&2
  exit 1
fi

echo "Using ${IMAGE} (${PLATFORM})"
docker pull --platform "${PLATFORM}" "${IMAGE}"

USER_ARGS=()
if id -u >/dev/null 2>&1; then
  USER_ARGS=(-u "$(id -u):$(id -g)")
fi

mkdir -p "${OUT_DIR}"

docker run --rm \
  --platform "${PLATFORM}" \
  "${USER_ARGS[@]}" \
  -v "${REPO_ROOT}:/work" \
  -w /work/tools/wii-gx-demo \
  -e DEVKITPRO=/opt/devkitpro \
  -e DEVKITPPC=/opt/devkitpro/devkitPPC \
  "${IMAGE}" \
  bash -lc '
    set -euo pipefail
    if [[ -f /opt/devkitpro/devkit-env ]]; then
      # shellcheck disable=SC1091
      source /opt/devkitpro/devkit-env
    elif [[ -f /etc/profile.d/devkit-env.sh ]]; then
      # shellcheck disable=SC1091
      source /etc/profile.d/devkit-env.sh
    fi
    export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
    export DEVKITPPC="${DEVKITPPC:-${DEVKITPRO}/devkitPPC}"
    export PATH="${DEVKITPPC}/bin:${DEVKITPRO}/tools/bin:${PATH}"
    echo "CC=$(command -v powerpc-eabi-gcc)"
    echo "LD=$(command -v powerpc-eabi-ld)"
    powerpc-eabi-gcc -v 2>&1 | tail -3
    make clean
    # Let powerpc-eabi-gcc drive the link step (do not set LD=powerpc-eabi-ld;
    # that makes -mhard-float look like an ld emulation).
    make -j"$(nproc)"
  '

cp -f "${DEMO_DIR}/boot.dol" "${OUT_DIR}/boot.dol"
cp -f "${DEMO_DIR}/boot.elf" "${OUT_DIR}/boot.elf" 2>/dev/null || true

# Stock elf2dol emits unaligned section sizes; modern Dolphin rejects those
# with "Failed to init core" / "No se ha podido iniciar el núcleo".
python3 "${REPO_ROOT}/scripts/pad-wii-dol.py" "${OUT_DIR}/boot.dol"
python3 "${REPO_ROOT}/scripts/validate-wii-dol.py" "${OUT_DIR}/boot.dol"
cp -f "${OUT_DIR}/boot.dol" "${DEMO_DIR}/boot.dol"

# Minimal HBC-style folder for SD sync (no data.win needed).
mkdir -p "${OUT_DIR}/apps/wii-gx-demo"
cp -f "${OUT_DIR}/boot.dol" "${OUT_DIR}/apps/wii-gx-demo/boot.dol"
cat > "${OUT_DIR}/apps/wii-gx-demo/meta.xml" <<'XML'
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<app version="1">
  <name>Butterscotch GX Demo</name>
  <coder>Butterscotch</coder>
  <version>0.1</version>
  <short_description>GX atlas/sprite stress demo</short_description>
  <long_description>Isolates Wii GX draw techniques without data.win.</long_description>
</app>
XML

echo ""
echo "Demo DOL: ${OUT_DIR}/boot.dol"
echo "Run with: ./scripts/run-wii-gx-demo.sh"
