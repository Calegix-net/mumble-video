#!/bin/bash
# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

# Builds the benchmark rig from nothing: a server profile and two client profiles that connect to it
# without a human present.
#
# ## The one hard part
#
# A Mumble client asks before trusting a server's certificate, and a headless client has nobody to ask,
# so it sits at "Not connected" for ever. The trust is stored per hostname and port, as the SHA-1
# fingerprint of the server's certificate, in the client's own `cert` table. So the rig starts the
# server once, reads the fingerprint off the socket with openssl, and writes that row into each
# client's database before the client has ever run. The client creates its tables with CREATE TABLE IF
# NOT EXISTS, so a database that contains only this one table is adopted rather than rejected.
#
# That is also why MV_HOST and MV_PORT are fixed: the pair is the key the trust is stored under.
#
# ## Everything else is settings
#
# A client that stops to ask a question measures nothing, so the wizards and consent prompts are marked
# as already seen. Nothing here is fabricated to make results look better - these are the same defaults
# a user who has clicked through the wizards once is running with.
#
# Safe to run again: it replaces the rig and leaves previous runs under out/ alone.
set -u
# shellcheck source=env.sh
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"

mv_require sqlite3 openssl python3 Xvfb ffmpeg tc unshare || exit 1
mv_check_build || exit 1

rm -rf "$MV_WORK/server" "$MV_WORK/clients"
mkdir -p "$MV_WORK/server" "$MV_WORK/clients" "$MV_OUT"

# --------------------------------------------------------------------------------------------------
# Python, for the summariser's percentiles. The only third-party dependency in the harness.
# --------------------------------------------------------------------------------------------------
if [ ! -x "$MV_PYTHON" ] && [ "$MV_PYTHON" = "$MV_WORK/venv/bin/python" ]; then
	echo "== making a virtual environment with numpy and meson =="
	python3 -m venv "$MV_WORK/venv" || exit 1
	"$MV_WORK/venv/bin/pip" install --quiet --upgrade pip numpy meson || exit 1
fi

"$MV_PYTHON" -c "import numpy" 2>/dev/null || {
	echo "$MV_PYTHON cannot import numpy" >&2
	exit 1
}

# --------------------------------------------------------------------------------------------------
# The server.
# --------------------------------------------------------------------------------------------------
cat > "$MV_WORK/server/mumble-server.ini" << INI
database=$MV_WORK/server/murmur.sqlite
logfile=$MV_WORK/server/server.log
host=$MV_HOST
port=$MV_PORT
users=100
welcometext=mumble-video benchmark rig
allowping=true
# The rig restarts clients between scenarios, and a ban would end a run silently rather than loudly.
autobanAttempts=0
INI

echo "== starting the server, so it generates its certificate =="
"$MV_SERVER" --foreground --ini "$MV_WORK/server/mumble-server.ini" > "$MV_WORK/server/first-run.log" 2>&1 &
SERVER_PID=$!
trap 'kill "$SERVER_PID" 2> /dev/null' EXIT

# Waited for rather than slept past: the certificate does not exist until the listener is up, and how
# long that takes depends on the machine.
DIGEST=""
for _ in $(seq 1 30); do
	sleep 1

	kill -0 "$SERVER_PID" 2>/dev/null || break

	DIGEST=$(openssl s_client -connect "$MV_HOSTPORT" -servername "$MV_HOST" < /dev/null 2>/dev/null \
		| openssl x509 -noout -fingerprint -sha1 2>/dev/null \
		| sed 's/.*=//; s/://g' \
		| tr 'A-Z' 'a-z')

	[ -n "$DIGEST" ] && break
done

if [ -z "$DIGEST" ]; then
	echo "could not read the server's certificate; last lines of its log:" >&2
	tail -20 "$MV_WORK/server/first-run.log" >&2
	exit 1
fi

echo "   server certificate sha1: $DIGEST"

# --------------------------------------------------------------------------------------------------
# The clients. One profile each, with the trust row written in.
# --------------------------------------------------------------------------------------------------
for pair in "sender:qa-mv1" "receiver:qa-mv2"; do
	name="${pair%%:*}"
	dir="$MV_WORK/clients/$name"

	mkdir -p "$dir/.config/Mumble/Mumble" "$dir/.local/share" "$dir/run"
	chmod 700 "$dir/run"

	sqlite3 "$dir/mumble.sqlite" \
		"CREATE TABLE IF NOT EXISTS \`cert\` (\`id\` INTEGER PRIMARY KEY AUTOINCREMENT, \`hostname\` TEXT, \`port\` INTEGER, \`digest\` TEXT);
		 CREATE UNIQUE INDEX IF NOT EXISTS \`cert_host_port\` ON \`cert\`(\`hostname\`,\`port\`);
		 INSERT OR REPLACE INTO \`cert\` (\`hostname\`, \`port\`, \`digest\`) VALUES ('$MV_HOST', $MV_PORT, '$DIGEST');" || exit 1

	# An identity certificate, or the client stops on a modal wizard it cannot be asked to dismiss.
	#
	# main.cpp generates one on first run, and the way it does that is `CertWizard::exec()` - a modal
	# dialog. On a virtual display with nobody watching, that is a client that starts, says nothing, and
	# never connects, which is a genuinely baffling thing to debug. The client stores its identity as a
	# base64 PKCS#12 blob in the `certificate` setting and reads it back with OpenSSL, so one made here
	# with openssl is indistinguishable from one it made itself.
	#
	# Self-signed and ten years, which is what the client's own wizard produces. It identifies a
	# benchmark client to a benchmark server and is not a secret: the whole rig is regenerated by
	# re-running this script.
	openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 3650 \
		-subj "/CN=$name@mumble-video-benchmark" \
		-keyout "$dir/identity-key.pem" -out "$dir/identity-cert.pem" 2> /dev/null || exit 1
	openssl pkcs12 -export -out "$dir/identity.p12" -inkey "$dir/identity-key.pem" \
		-in "$dir/identity-cert.pem" -name "$name" -passout pass: 2> /dev/null || exit 1

	CERTIFICATE=$(base64 -w0 "$dir/identity.p12") || exit 1

	"$MV_PYTHON" - "$dir" "$CERTIFICATE" << 'PY' || exit 1
import json
import sys

directory = sys.argv[1]

settings = {
    # Without this the client rejects the whole file - "key 'settings_version' not found" - and runs
    # with defaults, which means the database location below is ignored, which means the certificate
    # trust row written above is in a database nothing reads, which means it never connects. It is one
    # line and it is the difference between a rig that works and a rig that silently does not.
    "settings_version": 1,
    # The identity generated above, in the form the client itself writes: base64 PKCS#12. Without it
    # the client stops on a modal certificate wizard.
    "certificate": sys.argv[2],
    "misc": {
        "database_location": directory + "/mumble.sqlite",
        # Nothing in this rig can answer a question, and a client waiting for an answer measures nothing.
        "audio_wizard_has_been_shown": True,
        "viewed_server_ping_consent_message": True,
    },
    # Without this the client offers to restore a backup, having decided the last run crashed - which,
    # since the rig kills it between scenarios, it always has.
    "mumble_has_quit_normally": True,
    "video": {
        "video_wizard_shown": True,
        # 0 is "hardware H.264 when an encoder opens, VP9 in screen-content mode otherwise", which is
        # the default a user gets. inner-bench.sh overwrites this per run; 1 forces VP9.
        "screen_share_codec": 0,
    },
    "ui": {"quit_behavior": "AlwaysQuit"},
}

with open(directory + "/.config/Mumble/Mumble/mumble_settings.json", "w") as handle:
    json.dump(settings, handle, indent=2)
PY

	echo "   $name: trusts $(sqlite3 "$dir/mumble.sqlite" 'select hostname||":"||port from cert;')"
done

# --------------------------------------------------------------------------------------------------
# One connection each, before anything is measured.
#
# A client's first run is not like its later ones: it migrates its data directory, creates its
# database tables and generates its own identity certificate. Without this the first scenario of the
# first benchmark pays for all of that and usually fails outright, which is a confusing way to be told
# that the rig is new.
#
# The server's log is the signal, not the client's. A client that has not finished connecting accepts
# instructions and does nothing useful with them, and no client log line reliably says "connected" -
# where the server says "Authenticated" and names the user.
#
# Read from the console capture rather than from the logfile= the ini names, because --foreground sends
# the log to the console and leaves that file unwritten.
# --------------------------------------------------------------------------------------------------
echo "== connecting each client once, so its first run is behind it =="

# Separated by | and not by : because a display name *is* ":90", and ${x##*:} happily takes the
# colon with it - which produced "could not connect to display 90" and a Qt abort.
for triple in "sender|qa-mv1|$MV_SENDER_DISPLAY" "receiver|qa-mv2|$MV_RECEIVER_DISPLAY"; do
	name="${triple%%|*}"
	rest="${triple#*|}"
	user="${rest%%|*}"
	display="${rest##*|}"
	dir="$MV_WORK/clients/$name"

	Xvfb "$display" -screen 0 640x480x24 -extension GLX > "$MV_WORK/server/xvfb-$name.log" 2>&1 &
	XVFB_PID=$!
	sleep 2

	env HOME="$dir" XDG_RUNTIME_DIR="$dir/run" XDG_CONFIG_HOME="$dir/.config" \
		XDG_DATA_HOME="$dir/.local/share" DISPLAY="$display" QT_QPA_PLATFORM=xcb \
		"$MV_CLIENT" --skip-settings-backup-prompt "mumble://$user@$MV_HOSTPORT/" \
		> "$MV_WORK/server/warmup-$name.log" 2>&1 &
	CLIENT_PID=$!

	JOINED=0
	for _ in $(seq 1 30); do
		sleep 1

		if grep -q "$user.*Authenticated" "$MV_WORK/server/first-run.log" 2> /dev/null; then
			JOINED=1
			break
		fi
	done

	HOME="$dir" XDG_RUNTIME_DIR="$dir/run" timeout 10 "$MV_CLIENT" rpc quit > /dev/null 2>&1
	sleep 2
	kill "$CLIENT_PID" 2> /dev/null
	kill "$XVFB_PID" 2> /dev/null

	if [ "$JOINED" = "1" ]; then
		echo "   $name: connected and authenticated"
	else
		echo "   $name: NEVER CONNECTED. Last lines of its log:" >&2
		tail -15 "$MV_WORK/server/warmup-$name.log" >&2
		echo "   and of the server's:" >&2
		tail -10 "$MV_WORK/server/first-run.log" >&2
		exit 1
	fi
done

kill "$SERVER_PID" 2> /dev/null
wait "$SERVER_PID" 2> /dev/null
trap - EXIT

echo
echo "client: $MV_CLIENT"
echo "server: $MV_SERVER"
echo "work:   $MV_WORK"
echo "rig ready. Next: bench.sh smoke 20 auto clean"
