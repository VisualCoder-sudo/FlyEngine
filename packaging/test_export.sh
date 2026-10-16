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
# Assets in a folder beside the game (the default): it has to travel with the game.
if [[ -d "${WORK}/out/Assets" ]]; then cp -r "${WORK}/out/Assets" "${WORK}/run/Assets"; fi
if ldd "${WORK}/run/Game" | grep -q "not found"; then
    echo "FAIL: unresolved libraries:"; ldd "${WORK}/run/Game" | grep "not found"; exit 1
fi
if ldd "${WORK}/run/Game" | grep -Eq "box3d|libcurl"; then
    echo "FAIL: depends on a library from the build tree"; exit 1
fi

( cd "${WORK}/run" && HOME="${WORK}/home" XDG_CONFIG_HOME="${WORK}/home/.config" FLY_EXIT_AFTER_FRAMES=60 \
    timeout 120 ./Game > "${WORK}/log.txt" 2>&1 ) || { echo "FAIL: the game did not exit cleanly"; tail -20 "${WORK}/log.txt"; exit 1; }

EXTRA="$(ls -A "${WORK}/run" | grep -v -e '^Game$' -e '^Assets$' || true)"
if [[ -n "${EXTRA}" ]]; then echo "FAIL: the game wrote next to itself: ${EXTRA}"; exit 1; fi

# Without its Assets folder the game must still start once told to continue (and stop when told to quit).
if [[ -d "${WORK}/run/Assets" ]]; then
    rm -rf "${WORK}/run/Assets"
    ( cd "${WORK}/run" && HOME="${WORK}/home" XDG_CONFIG_HOME="${WORK}/home/.config" FLY_CONTENT_PROMPT=continue FLY_EXIT_AFTER_FRAMES=30 \
        timeout 120 ./Game > "${WORK}/log_noassets.txt" 2>&1 ) || { echo "FAIL: the game did not run without its Assets folder"; tail -20 "${WORK}/log_noassets.txt"; exit 1; }
    grep -q "content files missing" "${WORK}/log_noassets.txt" || { echo "FAIL: missing Assets folder was not noticed"; exit 1; }
fi

echo "OK: exported game ran from an empty folder ($(du -h "${WORK}/out/Game" | cut -f1))"
