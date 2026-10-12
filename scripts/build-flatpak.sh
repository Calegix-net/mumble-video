#!/bin/bash
# Builds the client flatpak and exports it as a single-file bundle.
#
# Run inside a privileged container with flatpak and flatpak-builder (flatpak-builder sandboxes each
# module build with bubblewrap, which needs the privileges). The state directory holds the runtimes
# and build cache and should be a persistent mount: cold it downloads a couple of gigabytes, warm it
# rebuilds only what changed.
#
# RELEASE_VERSION, if set, is the version the client reports itself as (scripts/installer-version.sh
# --release). The build cannot work it out itself: the manifest leaves .git out of the sandbox.
#
# Usage: build-flatpak.sh <source-dir> <state-dir> <output.flatpak>

set -euo pipefail

SRC_DIR="${1:?usage: build-flatpak.sh <source-dir> <state-dir> <output.flatpak>}"
STATE_DIR="${2:?usage: build-flatpak.sh <source-dir> <state-dir> <output.flatpak>}"
OUT="${3:?usage: build-flatpak.sh <source-dir> <state-dir> <output.flatpak>}"

MANIFEST="${SRC_DIR}/flatpak/com.calegix.MumbleVideo.yml"
RUNTIME_VERSION="$(grep '^runtime-version:' "${MANIFEST}" | sed 's/.*"\(.*\)"/\1/')"

# flatpak-builder has no way to add a config option from the command line, so the version goes into a
# copy of the manifest. The copy sits next to the original because the manifest's source path (..) is
# relative to wherever the manifest is.
if [[ -n "${RELEASE_VERSION:-}" ]]; then
    if [[ ! "${RELEASE_VERSION}" =~ ^[0-9A-Za-z][0-9A-Za-z.+_-]*$ ]]; then
        echo "build-flatpak: unusable RELEASE_VERSION '${RELEASE_VERSION}'" >&2
        exit 1
    fi
    RELEASE_MANIFEST="${SRC_DIR}/flatpak/.release-version.com.calegix.MumbleVideo.yml"
    trap 'rm -f "${RELEASE_MANIFEST}"' EXIT
    sed "s/^\( *- -Drelease-version=\)\$/\1${RELEASE_VERSION}/" "${MANIFEST}" > "${RELEASE_MANIFEST}"
    if ! grep -qxF -- "      - -Drelease-version=${RELEASE_VERSION}" "${RELEASE_MANIFEST}"; then
        echo "build-flatpak: no empty -Drelease-version= line in ${MANIFEST} to fill in" >&2
        exit 1
    fi
    MANIFEST="${RELEASE_MANIFEST}"
fi
echo "Release version: ${RELEASE_VERSION:-(worked out by the build)}"

export FLATPAK_USER_DIR="${STATE_DIR}/flatpak-user"
mkdir -p "${FLATPAK_USER_DIR}" "${STATE_DIR}/builder"

flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo

flatpak install --user -y --noninteractive flathub \
    "org.kde.Platform//${RUNTIME_VERSION}" "org.kde.Sdk//${RUNTIME_VERSION}"

# --delete-build-dirs keeps the footprint down: each module's unpacked tree is several hundred MB and
# is of no use once the module is built.
flatpak-builder --user --force-clean --disable-rofiles-fuse --delete-build-dirs \
    --state-dir="${STATE_DIR}/builder" \
    --repo="${STATE_DIR}/repo" \
    "${STATE_DIR}/build" "${MANIFEST}"

# --runtime-repo lets `flatpak install <bundle>` fetch org.kde.Platform from Flathub itself instead of
# failing with "requires the runtime ... which was not found" on a machine that has never had it.
flatpak build-bundle --runtime-repo=https://flathub.org/repo/flathub.flatpakrepo "${STATE_DIR}/repo" "${OUT}" com.calegix.MumbleVideo

echo "built $(du -h "${OUT}" | cut -f1) flatpak: ${OUT}"
