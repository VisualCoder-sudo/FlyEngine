#!/usr/bin/env bash
#
# Builds the FlyScript SDK + sample scripts into FlyScript.dll.
#
#   ./ScriptingSDK/build_sdk.sh [project_path]
#
# If project_path is given, the result is copied to <project_path>/Scripts/,
# which is where CoreCLRHost looks for it.
#
# The engine embeds CoreCLR only on Windows today (see
# src/Engine/Backend/CoreCLRHost.cpp), so this script is not needed to run the
# editor on Linux. It is kept working cross-platform anyway: the Linux CLR host
# only needs this assembly to exist, and having one build command that works
# everywhere keeps that port from having to rediscover the RID conventions.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK_DIR="${SCRIPT_DIR}/FlyScript"

# Honour DOTNET_ROOT, then fall back to whatever `dotnet` is on PATH, then to
# the standard per-user install location. Hardcoding a path here meant the
# script only ever worked on the machine it was written on.
if [[ -n "${DOTNET_ROOT:-}" && -x "${DOTNET_ROOT}/dotnet" ]]; then
    DOTNET="${DOTNET_ROOT}/dotnet"
elif command -v dotnet >/dev/null 2>&1; then
    DOTNET="$(command -v dotnet)"
elif [[ -x "${HOME}/.dotnet/dotnet" ]]; then
    DOTNET="${HOME}/.dotnet/dotnet"
else
    echo "[build_sdk] error: no dotnet found." >&2
    echo "  Install the .NET 8 SDK, or set DOTNET_ROOT to its location." >&2
    exit 1
fi

# Runtime identifier. The assembly is IL and identical either way; the RID only
# affects which native host assets dotnet copies alongside it. Match the RID to
# the machine the editor will run on.
RID="${FLYSDK_RID:-}"
if [[ -z "${RID}" ]]; then
    case "$(uname -s)" in
        Linux)  RID="linux-x64" ;;
        Darwin) RID="osx-x64" ;;
        *)      RID="win-x64" ;;
    esac
    # linux-arm64 and friends report differently to uname; let the caller
    # override with FLYSDK_RID if the auto-detected default is wrong.
    if [[ "$(uname -s)" == "Linux" && "$(uname -m)" == "aarch64" ]]; then
        RID="linux-arm64"
    fi
fi

if [[ ! -f "${SDK_DIR}/FlyScript.csproj" ]]; then
    echo "[build_sdk] error: FlyScript.csproj not found at ${SDK_DIR}" >&2
    exit 1
fi

OUT_DIR="${SDK_DIR}/bin/Release/net8.0/publish"

echo "=== Building FlyScript SDK + sample scripts ==="
echo "  dotnet : ${DOTNET}"
echo "  rid    : ${RID}"
echo "  out    : ${OUT_DIR}"

"${DOTNET}" publish "${SDK_DIR}/FlyScript.csproj" \
    -c Release \
    -o "${OUT_DIR}" \
    --nologo \
    -r "${RID}" \
    --self-contained false

DLL="${OUT_DIR}/FlyScript.dll"
if [[ ! -f "${DLL}" ]]; then
    echo "[build_sdk] error: FlyScript.dll not found at ${DLL}" >&2
    exit 1
fi

echo "[build_sdk] built ${DLL}"

if [[ $# -ge 1 && -n "${1:-}" ]]; then
    PROJECT_PATH="$1"
    SCRIPTS_DIR="${PROJECT_PATH}/Scripts"
    mkdir -p "${SCRIPTS_DIR}"
    cp -f "${DLL}" "${SCRIPTS_DIR}/FlyScript.dll"
    echo "[build_sdk] copied to ${SCRIPTS_DIR}/FlyScript.dll"
fi

echo
echo "=== Build complete ==="
