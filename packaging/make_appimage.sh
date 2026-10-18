#!/bin/sh
# Packs a Linux build directory (the export/<platform>/ folder from the CMake build) into Flyengine-x86_64.AppImage.
# Meant to run inside the oldest distro you want to support (e.g. debian:12) so the glibc requirement stays low:
#   podman run --rm -v <repo>:/src:ro -v <exe-dir>:/in:ro -v <outdir>:/out docker.io/library/debian:12 sh /src/packaging/make_appimage.sh
# Needs the runtime libraries installed so ldd can resolve them (apt install libx11-6 libxi6 libxcursor1 libssl3 zlib1g).
set -e
IN=${IN:-/in}; OUT=${OUT:-/out}; SRC=${SRC:-/src}
apt-get update -qq
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq wget file patchelf binutils libx11-6 libxi6 libxcursor1 libssl3 zlib1g >/dev/null

A=/tmp/Flyengine.AppDir
rm -rf $A && mkdir -p $A/usr/bin $A/usr/lib $A/usr/share/flyengine
find $IN -maxdepth 1 -type f -exec cp {} $A/usr/bin/ \;
cp -r $IN/assets $IN/ScriptingSDK $A/usr/share/flyengine/

# Bundle every library the binaries need except the ones that must come from the host: libc and friends, the C++
# runtime (the host's is newer and GPU drivers link against it), and the graphics stack.
SKIP='/(ld-linux|libc|libm|libdl|libpthread|librt|libutil|libresolv|libstdc\+\+|libgcc_s|libvulkan|libGL|libEGL|libGLX|libOpenGL|libdrm|libnvidia.*)\.so'
for b in $A/usr/bin/*; do ldd "$b"; done | awk '/=> \//{print $3}' | sort -u | grep -Ev "$SKIP" |
while read -r lib; do cp -L "$lib" $A/usr/lib/; done
for b in $A/usr/bin/*; do patchelf --set-rpath '$ORIGIN/../lib' "$b"; done
for l in $A/usr/lib/*.so*; do patchelf --set-rpath '$ORIGIN' "$l" 2>/dev/null || true; done

# The engine writes Captures/, Recordings/, shaders/, crashlog.txt ... into its working directory, so run from a
# per-user data folder instead of wherever the AppImage was launched (e.g. Downloads). Relative path arguments
# are made absolute first.
cat > $A/AppRun <<'R'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
ORIG="$PWD"
DATA="${XDG_DATA_HOME:-$HOME/.local/share}/flyengine"
if mkdir -p "$DATA" 2>/dev/null; then
    for a in "$@"; do
        case "$a" in
            -*) set -- "$@" "$a" ;;
            /*) set -- "$@" "$a" ;;
            *) if [ -e "$ORIG/$a" ]; then set -- "$@" "$(readlink -f "$ORIG/$a")"; else set -- "$@" "$a"; fi ;;
        esac
        shift
    done
    cd "$DATA"
fi
exec "$HERE/usr/bin/Flyengine" "$@"
R
chmod +x $A/AppRun
cat > $A/flyengine.desktop <<D
[Desktop Entry]
Type=Application
Name=Flyengine
Comment=3D game engine and editor
Exec=Flyengine
Icon=flyengine
Terminal=false
Categories=Development;IDE;Graphics;
D
cp $SRC/assets/icons/hicolor/256x256/apps/flyengine.png $A/flyengine.png

wget -q -O /tmp/appimagetool https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
chmod +x /tmp/appimagetool
ARCH=x86_64 /tmp/appimagetool --appimage-extract-and-run --no-appstream $A $OUT/Flyengine-x86_64.AppImage
