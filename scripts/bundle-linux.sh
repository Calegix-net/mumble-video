#!/bin/bash
# Assembles a self-contained Linux client directory from a native build.
#
# A Linux desktop offers no stable ABI across distributions, so almost everything the client links has
# to travel with it. Dependencies are resolved with ldd and copied into lib/, and a launcher sets
# LD_LIBRARY_PATH and the Qt plugin path rather than relying on rpath, which would have to be patched
# into every binary.
#
# Usage: bundle-linux.sh <build-dir> <output-dir>

set -euo pipefail

BUILD_DIR="${1:?usage: bundle-linux.sh <build-dir> <output-dir>}"
OUT_DIR="${2:?usage: bundle-linux.sh <build-dir> <output-dir>}"

QT_PLUGIN_DIR="$(qmake6 -query QT_INSTALL_PLUGINS 2>/dev/null || echo /usr/lib64/qt6/plugins)"

rm -rf "${OUT_DIR}"
mkdir -p "${OUT_DIR}/bin" "${OUT_DIR}/lib" "${OUT_DIR}/plugins"

cp "${BUILD_DIR}/mumble" "${OUT_DIR}/bin/"

# Positional-audio plugins, loaded at runtime by name.
find "${BUILD_DIR}/plugins" -name '*.so' -exec cp {} "${OUT_DIR}/plugins/" \; 2>/dev/null || true

# Qt plugins are dlopen'd, so they appear in no dependency list and must be named explicitly. Omitting
# platforms/ leaves the client unable to start at all; omitting multimedia/ leaves it running with an
# empty camera list, which is how an earlier bundle shipped with video that could never work.
for category in platforms multimedia tls imageformats iconengines platformthemes \
                platforminputcontexts xcbglintegrations wayland-decoration-client \
                wayland-graphics-integration-client wayland-shell-integration \
                networkinformation sqldrivers generic; do
    if [ -d "${QT_PLUGIN_DIR}/${category}" ]; then
        mkdir -p "${OUT_DIR}/plugins/${category}"
        cp "${QT_PLUGIN_DIR}/${category}"/*.so "${OUT_DIR}/plugins/${category}/" 2>/dev/null || true
    fi
done

# Resolve dependencies over everything placed above, Qt plugins included, since a plugin can need
# libraries the executable never references. Repeated until nothing new turns up, because each library
# copied in can itself pull in more.
copy_deps() {
    local added=0 lib

    while IFS= read -r -d '' file; do
        while read -r lib; do
            [ -n "${lib}" ] || continue
            [ -f "${OUT_DIR}/lib/$(basename "${lib}")" ] && continue

            case "$(basename "${lib}")" in
                # Anything binding the bundle to this kernel or this machine's graphics stack must come
                # from the host at runtime, not from here.
                ld-linux*|libc.so*|libm.so*|libdl.so*|libpthread.so*|librt.so*|libresolv.so*) continue ;;
                libGLX*|libGL.so*|libEGL.so*|libGLdispatch*|libdrm*|libgbm*) continue ;;
            esac

            cp -L "${lib}" "${OUT_DIR}/lib/" 2>/dev/null && added=1
        done < <(ldd "${file}" 2>/dev/null | awk '/=> \// { print $3 }')
    done < <(find "${OUT_DIR}" -type f \( -name '*.so*' -o -perm -u+x \) -print0)

    return ${added}
}

while ! copy_deps; do :; done

# Debug symbols are useless without the matching build tree and roughly double the download.
find "${OUT_DIR}" -type f \( -name '*.so*' -o -name mumble \) -exec strip --strip-unneeded {} + 2>/dev/null || true

cat > "${OUT_DIR}/mumble" <<'LAUNCHER'
#!/bin/bash
# Runs the bundled client against the bundled libraries.
here="$(dirname "$(readlink -f "$0")")"
export LD_LIBRARY_PATH="${here}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export QT_PLUGIN_PATH="${here}/plugins"
exec "${here}/bin/mumble" "$@"
LAUNCHER
chmod +x "${OUT_DIR}/mumble"

# Stamped so a published archive says which commit produced it. A bundle was once built from a stale
# tree and shipped as new - byte-identical to the previous one - and nothing in it could have revealed
# that. The binary's own checksum is recorded for the same reason.
{
    echo "commit:      $(git -C "$(dirname "$0")/.." rev-parse HEAD 2>/dev/null || echo unknown)"
    echo "describe:    $(git -C "$(dirname "$0")/.." describe --always --dirty 2>/dev/null || echo unknown)"
    echo "built:       $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "mumble.sha256: $(sha256sum "${OUT_DIR}/bin/mumble" | cut -d" " -f1)"
} > "${OUT_DIR}/BUILD-INFO.txt"

cat "${OUT_DIR}/BUILD-INFO.txt"
echo "bundled $(find "${OUT_DIR}/lib" -name '*.so*' | wc -l) libraries, $(du -sh "${OUT_DIR}" | cut -f1) total"
