#!/bin/sh
# Starts the Mumble Video server. On first run (no /data/mumble-server.ini yet) it writes a config
# from the environment variables below; after that the file in /data is yours to edit and the
# variables are ignored, except SUPERUSER_PASSWORD, which is applied on every start when set.
#
#   PORT                 port for TCP and UDP          (default 64738)
#   SERVER_PASSWORD      password users need to join   (default: none, open server)
#   WELCOME_TEXT         message shown on connect
#   MAX_USERS            user limit                    (default 100)
#   SERVER_NAME          name of the root channel      (default "Mumble Video")
#   SUPERUSER_PASSWORD   password for the SuperUser admin account (default: unchanged)
set -eu

DATA=/data
INI="$DATA/mumble-server.ini"
BIN=/usr/local/bin/mumble-server

if [ ! -w "$DATA" ]; then
    echo "entrypoint: $DATA is not writable. Mount a host folder there, e.g. -v ~/mumble-data:/data:Z" >&2
    exit 1
fi

# Escape characters that would break a double-quoted ini value.
ini_quote() {
    printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'
}

if [ ! -f "$INI" ]; then
    echo "entrypoint: no config found, writing default $INI"
    cat > "$INI" <<EOF
; Generated on first start. Edit freely; restart the container to apply changes.
; Full option reference: https://github.com/Calegix-net/mumble-video/blob/main/auxiliary_files/mumble-server.ini
database=$DATA/mumble-server.sqlite
port=${PORT:-64738}
serverpassword="$(ini_quote "${SERVER_PASSWORD:-}")"
welcometext="$(ini_quote "${WELCOME_TEXT:-Welcome to Mumble Video!}")"
users=${MAX_USERS:-100}
registerName="$(ini_quote "${SERVER_NAME:-Mumble Video}")"
bandwidth=558000

; Video limits advertised to clients. 0 = no limit.
;videobandwidth=20000000
;videostreamsperchannel=0
EOF
fi

if [ -n "${SUPERUSER_PASSWORD:-}" ]; then
    # Passed on stdin so the password never shows up in the process list.
    printf '%s\n' "$SUPERUSER_PASSWORD" | "$BIN" -i "$INI" --read-su-pw >/dev/null
    echo "entrypoint: SuperUser password set"
fi

exec "$BIN" --foreground -i "$INI" "$@"
