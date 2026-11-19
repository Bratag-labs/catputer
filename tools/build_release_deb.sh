#!/bin/sh
# Build a Cardputer Zero .deb on a development host — no aarch64 toolchain
# needed. Runs an ordinary in-container build (same steps as on the device)
# in an arm64 Debian container via docker + qemu, then packages the binary
# with tools/build_deb.py.
#
# Usage:  tools/build_release_deb.sh
# Output: build/deb/catputer_<version>_arm64.deb
# Needs:  docker, qemu-user (binfmt for linux/arm64)
set -eu
cd "$(dirname "$0")/.."

IMAGE=${IMAGE:-debian:bookworm}
BUILD_DIR=build/cmake-arm64
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM

echo "== building in $IMAGE (arm64) =="
docker run --rm --platform linux/arm64 \
  -v "$PWD":/src -w /src -v "$WORK":/out \
  "$IMAGE" sh -c '
    set -eu
    [ "$(uname -m)" = "aarch64" ] || {
      echo "error: container is not arm64 (qemu/binfmt not active?)" >&2; exit 1; }
    apt-get update -qq
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
      build-essential cmake libsdl2-dev >/dev/null
    cmake -S . -B '"$BUILD_DIR"' -DCMAKE_BUILD_TYPE=Release
    cmake --build '"$BUILD_DIR"' -j "$(nproc)"
    ctest --test-dir '"$BUILD_DIR"' --output-on-failure
    strip '"$BUILD_DIR"'/lofi_native
    cp '"$BUILD_DIR"'/lofi_native /out/lofi_native
  '

echo "== packaging =="
python3 tools/build_deb.py --binary "$WORK/lofi_native" --arch arm64 --no-strip
ls -l build/deb/
