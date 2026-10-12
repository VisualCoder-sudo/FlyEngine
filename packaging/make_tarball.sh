#!/usr/bin/env bash
#
# Builds a relocatable Linux tarball of the engine.
#
#   ./packaging/make_tarball.sh [build_dir] [output_dir]
#
# Defaults to build/ and dist/. Produces flyengine-linux-x86_64.tar.gz
# containing a /usr/local tree, so it can be unpacked anywhere:
#
#   tar -xzf flyengine-linux-x86_64.tar.gz
#   sudo cp -r usr/local/* /usr/local/
#   sudo update-desktop-database /usr/local/share/applications
#   gtk-update-icon-cache -f -t /usr/local/share/icons/hicolor
#   update-mime-database /usr/local/share/mime
#
# The unpacked binary runs from any working directory: it locates its data
# relative to its own path (usr/local/share/flyengine), not the CWD. See
# platform::ResolveAsset in src/Engine/Platform/Platform.cpp.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${1:-${REPO_ROOT}/build}"
OUT_DIR="${2:-${REPO_ROOT}/dist}"

if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    echo "error: ${BUILD_DIR} is not a configured CMake build directory." >&2
    echo "  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release" >&2
    exit 1
fi

# A debug build carries ~30 MB of DWARF that has no business in a
# distribution. Warn rather than fail, since a developer may just be testing
# the script against a debug build.
BUILD_TYPE="$(sed -n 's/^CMAKE_BUILD_TYPE:STRING=//p' "${BUILD_DIR}/CMakeCache.txt")"
if [[ "${BUILD_TYPE}" != "Release" && "${BUILD_TYPE}" != "RelWithDebInfo" && "${BUILD_TYPE}" != "MinSizeRel" ]]; then
    echo "warning: build type is '${BUILD_TYPE:-unset}', not Release." >&2
    echo "         The tarball will be much larger than it needs to be." >&2
fi

STAGE="$(mktemp -d)"
trap 'rm -rf "${STAGE}"' EXIT

echo "=== Installing into a staging tree ==="
DESTDIR="${STAGE}" cmake --install "${BUILD_DIR}" >/dev/null

PREFIX="${STAGE}/usr/local"

if [[ ! -x "${PREFIX}/bin/Flyengine" ]]; then
    echo "error: ${PREFIX}/bin/Flyengine was not installed." >&2
    exit 1
fi

# Vendored subprojects (Box3D, libcurl) install their own headers, static
# libraries, CMake package configs and pkg-config files. Those are development
# files: nobody building against this tarball needs them, and they are tens of
# megabytes. Drop them so the tarball is runtime-only.
echo "=== Stripping development files ==="
rm -rf "${PREFIX}/include" \
       "${PREFIX}/lib" \
       "${PREFIX}/share/pkgconfig" \
       "${PREFIX}/share/cmake" \
       "${PREFIX}/share/man"
# Static CRT / import libraries have no place in a Linux runtime tree either.
find "${PREFIX}" -name '*.a' -delete 2>/dev/null || true
find "${PREFIX}" -name '*.pdb' -delete 2>/dev/null || true

# Anything the engine never reads at runtime. Kept as a list rather than a
# blanket delete so a future data directory is not silently dropped.
for junk in \
    "share/flyengine/assets/Misc" \
    ; do
    if [[ -d "${PREFIX}/${junk}" ]]; then
        echo "  removing ${junk}"
        rm -rf "${PREFIX:?}/${junk}"
    fi
done

mkdir -p "${OUT_DIR}"
ARCH="$(uname -m)"
TARBALL="${OUT_DIR}/flyengine-linux-${ARCH}.tar.gz"

echo "=== Creating ${TARBALL} ==="
tar -czf "${TARBALL}" -C "${STAGE}" usr

echo
echo "=== Contents ==="
tar -tzf "${TARBALL}" | grep -vE '/$' | sort
echo
echo "=== Size ==="
du -h "${TARBALL}"
echo
echo "=== Largest shipped files ==="
du -h "${PREFIX}"/share/flyengine/assets/Textures/*/* 2>/dev/null | sort -rh | head -5 || true

cat <<EOF

Built ${TARBALL}

Install it with:
  sudo tar -xzf $(basename "${TARBALL}") -C /
  sudo update-desktop-database /usr/local/share/applications
  sudo gtk-update-icon-cache -f -t /usr/local/share/icons/hicolor
  sudo update-mime-database /usr/local/share/mime

Or run it in place without installing:
  tar -xzf $(basename "${TARBALL}") && ./usr/local/bin/Flyengine
EOF
