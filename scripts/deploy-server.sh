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

IMAGE="${IMAGE:-ghcr.io/calegix-net/mumble-video-server:dev-latest}"
NAME="${NAME:-mumble-fork-server}"
DATA_DIR="${DATA_DIR:-$HOME/mumble-fork/server-data}"

if [ ! -f "${DATA_DIR}/mumble-server.ini" ]; then
    echo "deploy-server: ${DATA_DIR}/mumble-server.ini not found; set DATA_DIR to the server's data directory." >&2
    exit 1
fi

echo "deploy-server: pulling ${IMAGE}"
podman pull "${IMAGE}"

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
