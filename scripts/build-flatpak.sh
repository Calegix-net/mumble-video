#!/bin/bash
# Builds the client flatpak and exports it as a single-file bundle.
#
# Run inside a privileged container with flatpak and flatpak-builder (flatpak-builder sandboxes each
# module build with bubblewrap, which needs the privileges). The state directory holds the runtimes
# and build cache and should be a persistent mount: cold it downloads a couple of gigabytes, warm it
# rebuilds only what changed.
#
# Usage: build-flatpak.sh <source-dir> <state-dir> <output.flatpak>

set -euo pipefail

SRC_DIR="${1:?usage: build-flatpak.sh <source-dir> <state-dir> <output.flatpak>}"
STATE_DIR="${2:?usage: build-flatpak.sh <source-dir> <state-dir> <output.flatpak>}"
OUT="${3:?usage: build-flatpak.sh <source-dir> <state-dir> <output.flatpak>}"

MANIFEST="${SRC_DIR}/flatpak/com.calegix.MumbleVideo.yml"
RUNTIME_VERSION="$(grep '^runtime-version:' "${MANIFEST}" | sed 's/.*"\(.*\)"/\1/')"

export FLATPAK_USER_DIR="${STATE_DIR}/flatpak-user"
mkdir -p "${FLATPAK_USER_DIR}" "${STATE_DIR}/builder"

flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo

flatpak install --user -y --noninteractive flathub \
    "org.kde.Platform//${RUNTIME_VERSION}" "org.kde.Sdk//${RUNTIME_VERSION}"

flatpak-builder --user --force-clean --disable-rofiles-fuse \
    --state-dir="${STATE_DIR}/builder" \
    --repo="${STATE_DIR}/repo" \
    "${STATE_DIR}/build" "${MANIFEST}"

flatpak build-bundle "${STATE_DIR}/repo" "${OUT}" com.calegix.MumbleVideo

echo "built $(du -h "${OUT}" | cut -f1) flatpak: ${OUT}"
