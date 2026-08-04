#!/bin/bash
# Assembles a self-contained Windows client directory from a MinGW cross-build.
#
# Windows has no rpath and no ldd, so the DLLs a binary needs have to sit next to it and be worked out
# statically. This walks the import tables with objdump, following each dependency in turn, and copies
# anything that resolves inside the cross-sysroot. DLLs that do not resolve there are Windows' own and
# are deliberately left alone.
#
# Usage: bundle-windows.sh <build-dir> <output-dir>

set -euo pipefail

BUILD_DIR="${1:?usage: bundle-windows.sh <build-dir> <output-dir>}"
OUT_DIR="${2:?usage: bundle-windows.sh <build-dir> <output-dir>}"

SYSROOT=/usr/x86_64-w64-mingw32/sys-root/mingw
OBJDUMP=x86_64-w64-mingw32-objdump

rm -rf "${OUT_DIR}"
mkdir -p "${OUT_DIR}"

cp "${BUILD_DIR}/mumble.exe" "${OUT_DIR}/"

# The positional-audio plugins, which the client loads at runtime from a plugins/ directory beside it.
mkdir -p "${OUT_DIR}/plugins"
find "${BUILD_DIR}/plugins" -name '*.dll' -exec cp {} "${OUT_DIR}/plugins/" \; 2>/dev/null || true

# Qt plugins. These are dlopen'd by name, so nothing in the import tables points at them and the
# dependency walk below cannot discover them - they have to be listed. Without platforms/ the client
# exits at startup with "could not find or load the Qt platform plugin"; without multimedia/ the camera
# list comes back empty, which is the failure that shipped in an earlier Linux bundle.
for category in platforms styles imageformats iconengines tls multimedia networkinformation generic; do
    src="${SYSROOT}/lib/qt6/plugins/${category}"
    if [ -d "${src}" ]; then
        mkdir -p "${OUT_DIR}/${category}"
        cp "${src}"/*.dll "${OUT_DIR}/${category}/" 2>/dev/null || true
    fi
done

# Only the SQLite driver. The client keeps its local database in SQLite, and the other drivers drag in
# client libraries that are not here and would never load - the PostgreSQL one alone wants LIBPQ.dll.
mkdir -p "${OUT_DIR}/sqldrivers"
cp "${SYSROOT}/lib/qt6/plugins/sqldrivers/qsqlite.dll" "${OUT_DIR}/sqldrivers/" 2>/dev/null || true

# Walk imports breadth-first over everything placed so far, including the Qt plugins, since a plugin can
# pull in libraries the executable itself never references.
declare -A seen
queue=()

while IFS= read -r -d '' file; do
    queue+=("${file}")
# The parentheses matter: without them -print0 binds only to the second -name and the executable, which
# is the one binary whose imports certainly need walking, is silently skipped.
done < <(find "${OUT_DIR}" \( -name '*.exe' -o -name '*.dll' \) -print0)

while [ ${#queue[@]} -gt 0 ]; do
    current="${queue[0]}"
    queue=("${queue[@]:1}")

    while read -r dll; do
        [ -n "${dll}" ] || continue
        [ -n "${seen[${dll}]:-}" ] && continue
        seen[${dll}]=1

        # Resolved against the sysroot and the build tree. A miss means it is a system DLL that Windows
        # provides. The build tree is searched too because some dependencies (speexdsp, rnnoise, spdlog)
        # are built here rather than installed.
        found=""
        for dir in "${SYSROOT}/bin" "${SYSROOT}/lib"; do
            if [ -f "${dir}/${dll}" ]; then
                found="${dir}/${dll}"
                break
            fi
        done

        # The build tree is searched recursively: dependencies built as part of this project land in
        # per-target subdirectories rather than next to the executable.
        if [ -z "${found}" ]; then
            found="$(find "${BUILD_DIR}" -name "${dll}" -type f -print -quit)"
        fi

        if [ -n "${found}" ]; then
            cp -n "${found}" "${OUT_DIR}/"
            queue+=("${OUT_DIR}/${dll}")
        fi
    done < <("${OBJDUMP}" -p "${current}" 2>/dev/null | awk '/DLL Name:/ { print $3 }')
done

# Debug symbols roughly double the download and are of no use without the matching build tree, so they
# come off here rather than at compile time - a stripped build would also strip what the crash reporter
# needs when debugging locally.
find "${OUT_DIR}" \( -name '*.exe' -o -name '*.dll' \) -exec x86_64-w64-mingw32-strip --strip-unneeded {} + 2>/dev/null || true

echo "bundled $(find "${OUT_DIR}" -name '*.dll' | wc -l) DLLs alongside mumble.exe"
