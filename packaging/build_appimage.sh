#!/bin/sh
# Builds the Linux release: compiles inside Debian 12 (old glibc/OpenSSL, so it runs on most distros) and packs
# export/Flyengine-x86_64.AppImage. Run from the repo root on any host with podman:
#   sh packaging/build_appimage.sh
# The build tree lives in build-appimage/ (gitignored) so repeat runs are incremental.
set -e
REPO=$(cd "$(dirname "$0")/.." && pwd)
IMAGE=docker.io/library/debian:12
mkdir -p "$REPO/export" "$REPO/build-appimage/stage"

podman run --rm -v "$REPO":/src -w /src "$IMAGE" sh -ec '
    apt-get update -qq
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
        build-essential cmake ninja-build pkg-config git ca-certificates perl \
        libasound2-dev libx11-dev libxrandr-dev libxi-dev libxcursor-dev libxinerama-dev libxkbcommon-dev \
        libgl1-mesa-dev libglib2.0-dev libvulkan-dev libudev-dev libssl-dev zlib1g-dev >/dev/null
    cmake -S . -B build-appimage -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DFLY_EXE_DIR=/src/build-appimage/stage \
        -DFLYENGINE_SOKOL_SHDC=/src/tools/sokol-shdc/sokol-shdc
    cmake --build build-appimage
'

podman run --rm -v "$REPO":/src:ro -v "$REPO/build-appimage/stage":/in:ro -v "$REPO/export":/out "$IMAGE" \
    sh /src/packaging/make_appimage.sh
