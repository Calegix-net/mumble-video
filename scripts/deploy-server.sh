#!/bin/bash
# Pulls the published server image and (re)creates the container. Idempotent: safe to run for the first
# deploy or for every update. Configuration is by environment variable so the same script works on any
# host:
#
#   IMAGE      full image ref to run        (default: ghcr.io/calegix-net/mumble-video-server:dev-latest)
#   NAME       container name               (default: mumble-fork-server)
#   DATA_DIR   host dir holding the .ini    (default: $HOME/mumble-fork/server-data)
#
# The data directory must contain mumble-server.ini; it is mounted at /data, where the image's
# entrypoint expects it. Host networking is used so UDP source addresses match the client's flow.
set -euo pipefail

# Host networking means the HOST's socket limits apply to the server's UDP socket. The server asks
# for 4 MB buffers so a whole frame burst (up to ~400 datagrams) fits; a stock Linux host clamps
# that to ~416 KB via net.core.rmem_max/wmem_max (default 212992) without any error, and the
# result is 0.35% of video dropped at the socket and black tiles on every viewer. Refuse to leave
# that silent. Fix: sysctl net.core.rmem_max=8388608 net.core.wmem_max=8388608 (persist it in
# /etc/sysctl.d/), then re-run this script so the new container's socket picks it up.
WANT_BUF=$((4 * 1024 * 1024))
for k in net.core.rmem_max net.core.wmem_max; do
    have=$(sysctl -n "$k" 2>/dev/null || echo 0)
    if [ "${have:-0}" -lt "$WANT_BUF" ]; then
        echo "WARNING: $k=$have is below the ${WANT_BUF} bytes the server requests for video bursts;" >&2
        echo "         the kernel will clamp the socket silently and viewers will see dropped tiles." >&2
        echo "         sudo sysctl -w $k=8388608   (and persist it under /etc/sysctl.d/)" >&2
    fi
done

IMAGE="${IMAGE:-ghcr.io/calegix-net/mumble-video-server:dev-latest}"
NAME="${NAME:-mumble-fork-server}"
DATA_DIR="${DATA_DIR:-$HOME/mumble-fork/server-data}"

if [ ! -f "${DATA_DIR}/mumble-server.ini" ]; then
    echo "deploy-server: ${DATA_DIR}/mumble-server.ini not found; set DATA_DIR to the server's data directory." >&2
    exit 1
fi

echo "deploy-server: pulling ${IMAGE}"
if ! podman pull "${IMAGE}"; then
    # A locally-built image (e.g. localhost/...) has no registry to pull from; use it if present.
    if podman image exists "${IMAGE}"; then
        echo "deploy-server: pull failed but ${IMAGE} exists locally; using the local image."
    else
        echo "deploy-server: could not pull ${IMAGE} and it is not present locally." >&2
        exit 1
    fi
fi

echo "deploy-server: replacing container ${NAME}"
podman rm -f "${NAME}" 2>/dev/null || true
podman run -d \
    --name "${NAME}" \
    --network=host \
    --restart unless-stopped \
    -v "${DATA_DIR}:/data:z" \
    "${IMAGE}"

sleep 2
podman ps --filter "name=${NAME}" --format '{{.Names}} {{.Status}} {{.Image}}'

# The old container is already gone by here, so a container that starts and immediately crashes (bad
# ini, a runtime library the image is missing) is a hard outage that --restart would only busy-loop.
# Fail loudly with the logs rather than exiting 0 on a dead server.
if [ "$(podman inspect -f '{{.State.Running}}' "${NAME}" 2>/dev/null)" != "true" ]; then
    echo "deploy-server: ${NAME} is not running after start - recent logs:" >&2
    podman logs --tail 40 "${NAME}" >&2 2>/dev/null || true
    exit 1
fi

echo "deploy-server: done"
