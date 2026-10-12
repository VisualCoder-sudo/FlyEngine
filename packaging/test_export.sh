#!/usr/bin/env bash
#
# Exports a project with flyexport, then runs the result ALONE in an empty folder (fresh HOME) for a few frames.
# Checks: clean exit, no unresolved or unusual library dependencies, nothing written next to the executable.
#
#   packaging/test_export.sh <project dir> [kit dir] [build dir]
#
# Needs a display + GPU like the other rendering tests. Set FLY_SCREENSHOT=<png> to keep the last frame.
set -euo pipefail

PROJECT="${1:?usage: test_export.sh <project dir> [kit dir] [build dir]}"
KIT="${2:-}"
BUILD="${3:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/build-vk}"

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT
mkdir -p "${WORK}/out" "${WORK}/run" "${WORK}/home"

if [[ -n "${KIT}" ]]; then export FLYENGINE_KIT_DIR="${KIT}"; fi
"${BUILD}/flyexport" "${PROJECT}" --out "${WORK}/out" --name Game

cp "${WORK}/out/Game" "${WORK}/run/"
if ldd "${WORK}/run/Game" | grep -q "not found"; then
    echo "FAIL: unresolved libraries:"; ldd "${WORK}/run/Game" | grep "not found"; exit 1
fi
if ldd "${WORK}/run/Game" | grep -Eq "box3d|libcurl"; then
    echo "FAIL: depends on a library from the build tree"; exit 1
fi

( cd "${WORK}/run" && HOME="${WORK}/home" XDG_CONFIG_HOME="${WORK}/home/.config" FLY_EXIT_AFTER_FRAMES=60 \
    timeout 120 ./Game > "${WORK}/log.txt" 2>&1 ) || { echo "FAIL: the game did not exit cleanly"; tail -20 "${WORK}/log.txt"; exit 1; }

EXTRA="$(ls -A "${WORK}/run" | grep -v '^Game$' || true)"
if [[ -n "${EXTRA}" ]]; then echo "FAIL: the game wrote next to itself: ${EXTRA}"; exit 1; fi

echo "OK: exported game ran from an empty folder ($(du -h "${WORK}/out/Game" | cut -f1))"
