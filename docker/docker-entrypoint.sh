#!/bin/sh
# Entrypoint for the Mumble server container image.
#
# Makes the published image usable with nothing but `docker run -v mumble-data:/data -p 64738:64738/tcp
# -p 64738:64738/udp ghcr.io/<owner>/mumble-video-server`: on first run it seeds a default config into the
# mounted /data volume, and it makes that volume writable by the unprivileged server user. It starts as
# root only to do those two things; the server itself then drops to the 'mumble' user via the config's
# `uname` option, so the long-running process is unprivileged.
set -e

CONFIG=/data/mumble-server.ini
DEFAULT=/etc/mumble/mumble-server.ini.default

mkdir -p /data

if [ ! -f "$CONFIG" ]; then
	echo "mumble-server: no $CONFIG yet - seeding the default. Edit it in your /data volume and restart to configure."
	cp "$DEFAULT" "$CONFIG"
fi

# The volume is often a freshly created/mounted directory owned by root; hand it to the server user so the
# database and certificate can be written. Harmless if already correct; tolerated if the fs is read-only.
if [ "$(id -u)" = "0" ]; then
	chown -R mumble:mumble /data 2>/dev/null || true
fi

# The SuperUser password can be set out-of-band without editing the config:
#   docker run --rm -e MUMBLE_SUPERUSER_PASSWORD=... -v mumble-data:/data <image> (one-shot), or exec:
if [ -n "${MUMBLE_SUPERUSER_PASSWORD:-}" ]; then
	echo "mumble-server: setting SuperUser password from MUMBLE_SUPERUSER_PASSWORD"
	/usr/local/bin/mumble-server -i "$CONFIG" -supw "$MUMBLE_SUPERUSER_PASSWORD" || true
fi

# PID 1, --foreground so the container stays up; -ini the volume copy. Extra args pass through.
exec /usr/local/bin/mumble-server --foreground -i "$CONFIG" "$@"
