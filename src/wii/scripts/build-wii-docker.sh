#!/usr/bin/env bash
# Build PLATFORM=wii inside the official multi-arch devkitPro image.
# Prefer this on macOS so the host stays free of /opt/devkitpro.
#
# Image: docker.io/devkitpro/devkitppc (linux/arm64 and linux/amd64)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"

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
  echo "docker not found. Install Docker Desktop, then retry." >&2
  exit 1
fi

echo "Using ${IMAGE} (${PLATFORM})"
docker pull --platform "${PLATFORM}" "${IMAGE}"

USER_ARGS=()
if id -u >/dev/null 2>&1; then
  USER_ARGS=(-u "$(id -u):$(id -g)")
fi

docker run --rm \
  --platform "${PLATFORM}" \
  "${USER_ARGS[@]}" \
  -v "${REPO_ROOT}:/work" \
  -w /work \
  -e DEVKITPRO=/opt/devkitpro \
  -e DEVKITPPC=/opt/devkitpro/devkitPPC \
  "${IMAGE}" \
  bash -lc 'bash ./src/wii/scripts/build-wii.sh'
