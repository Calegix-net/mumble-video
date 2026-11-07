#!/bin/bash
# Runs the mv-attack client through a set of live scenarios against a real server, and checks the
# outcome of each against what the fixes on this branch promise.
#
# Structure mirrors scripts/benchmark: an outer invocation unshares a network namespace and re-enters
# this script, so the whole thing runs on an isolated loopback that no other process shares and that is
# torn down with the namespace. Every scenario is one authenticated user doing something an ordinary
# user could do; the point is whether the server lets it have an effect it should not.
#
# Usage:
#   MV_ATTACK=/path/to/mv-attack MV_SERVER=/path/to/mumble-server bash run-attacks.sh
#
# Needs: unshare (user namespaces), openssl, python3. No display, no Qt, no GUI client.
set -u

MV_ATTACK="${MV_ATTACK:?set MV_ATTACK to the mv-attack binary}"
MV_SERVER="${MV_SERVER:?set MV_SERVER to the mumble-server binary}"
MV_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="${MV_ATTACK_WORK:-/tmp/mv-attack-work}"
PORT="${MV_ATTACK_PORT:-64799}"
HOST=127.0.0.1

# Re-enter inside a fresh network namespace, exactly as the benchmark rig does, unless already there.
if [ -z "${MV_IN_NETNS:-}" ]; then
	export MV_IN_NETNS=1 MV_ATTACK MV_SERVER WORK PORT HOST MV_SCRIPT_DIR
	exec unshare -r --net "$0" "$@"
fi

# A fresh namespace has loopback down; nothing connects until it is up. This is the single most common
# way a run in here silently measures nothing - see the benchmark rig's note on the same line.
ip link set lo up

rm -rf "$WORK"
mkdir -p "$WORK"

# A distinct certificate per client. Mumble identifies a user by the hash of its certificate, so two
# connections sharing one are the same identity as far as the server is concerned - which collides, and
# is why the benchmark rig gives every client its own. One shared cert here made every watcher look like
# the sender and no subscription was ever authorised for a second party.
mkdir -p "$WORK/certs"
cert_for() { # name -> echoes "certpath keypath", creating them once
	local name="$1"
	local c="$WORK/certs/$name.pem"
	local k="$WORK/certs/$name.key"
	if [ ! -f "$c" ]; then
		openssl req -x509 -newkey rsa:2048 -keyout "$k" -out "$c" -days 3650 -nodes \
			-subj "/CN=mv-$name" >> "$WORK/openssl.log" 2>&1
	fi
	echo "$c $k"
}

cat > "$WORK/server.ini" << INI
database=$WORK/murmur.sqlite
logfile=$WORK/server.log
host=$HOST
port=$PORT
users=100
welcometext=mv-attack test server
allowping=true
autobanAttempts=0
INI

echo "== starting the server =="
"$MV_SERVER" --foreground --ini "$WORK/server.ini" > "$WORK/server.stdout" 2>&1 &
SERVER_PID=$!
trap 'kill "$SERVER_PID" 2>/dev/null' EXIT

# Wait for the listener rather than sleeping a guessed interval.
up=0
for _ in $(seq 1 50); do
	if openssl s_client -connect "$HOST:$PORT" < /dev/null > /dev/null 2>&1; then
		up=1
		break
	fi
	sleep 0.1
done
if [ "$up" -ne 1 ]; then
	echo "server never came up; log follows" >&2
	tail -20 "$WORK/server.stdout" >&2
	exit 1
fi

# --- scenario driver -------------------------------------------------------------------------------

attack() { # name args...  -> writes $WORK/<name>.jsonl, with a cert of its own
	local name="$1"
	shift
	read -r c k <<< "$(cert_for "$name")"
	"$MV_ATTACK" --host "$HOST" --port "$PORT" --cert "$c" --key "$k" --name "$name" "$@" \
		> "$WORK/$name.jsonl" 2>"$WORK/$name.err"
}

# Starts a sender in the background, waits until it reports its own session id, and echoes that id so a
# watcher can be pointed at it. The sender keeps running for the duration given.
start_sender_get_session() { # name args...
	local name="$1"
	shift
	read -r c k <<< "$(cert_for "$name")"
	"$MV_ATTACK" --host "$HOST" --port "$PORT" --cert "$c" --key "$k" --name "$name" --role sender "$@" \
		> "$WORK/$name.jsonl" 2>"$WORK/$name.err" &
	echo $! > "$WORK/$name.pid"
	local session=""
	for _ in $(seq 1 100); do
		session=$("$MV_SCRIPT_DIR/attack-report.py" session "$WORK/$name.jsonl" 2>/dev/null)
		[ -n "$session" ] && [ "$session" != "0" ] && break
		sleep 0.05
	done
	echo "$session"
}

echo "== scenarios =="

# Each pairs a sender (often the attacker) with a watcher, gives them overlapping lifetimes, then the
# Python checker reads both JSONL files and decides pass or fail. Durations are generous: these are
# correctness checks, not timing measurements.

run_sender_watcher() { # label  sender-extra-args  watcher-extra-args
	local label="$1" sargs="$2" wargs="$3"
	rm -f "$WORK/$label.sender.jsonl" "$WORK/$label.watcher.jsonl"

	# shellcheck disable=SC2086
	local session
	session=$(start_sender_get_session "$label.sender" --duration-ms 6000 $sargs)
	if [ -z "$session" ] || [ "$session" = "0" ]; then
		echo "  $label: sender never synced" >&2
		cat "$WORK/$label.sender.err" >&2
		return
	fi

	# shellcheck disable=SC2086
	attack "$label.watcher" --role watcher --watch-session "$session" --duration-ms 4000 $wargs

	wait "$(cat "$WORK/$label.sender.pid")" 2>/dev/null
}

# 1. Baseline: an ordinary OpusAudio share reaches an ordinary watcher. If this does not pass, nothing
#    below means anything - it is the control that proves the path works at all.
run_sender_watcher baseline-audio "--codec opusaudio --stream-id 1" "--watch-stream 1"

# 2. The headline fix: a self-muted sender's OpusAudio must NOT reach the watcher.
run_sender_watcher muted-audio "--codec opusaudio --stream-id 1 --self-mute" "--watch-stream 1"

# 3. A self-deafened watcher must NOT receive a share's audio, even from an unmuted sender.
run_sender_watcher deaf-watcher "--codec opusaudio --stream-id 1" "--watch-stream 1 --self-deaf"

# 4. The fix must be narrow: a self-muted sender's PICTURE stream still reaches the watcher. Mute gates
#    sound, not video.
run_sender_watcher muted-picture "--codec vp9 --stream-id 1 --self-mute" "--watch-stream 1"

# 5. source_name is truncated by the server. The watcher observes the relayed VideoState and reports the
#    length it received; the checker asserts it is <= 512 although the sender sent 4000.
run_sender_watcher longname "--codec vp9 --stream-id 1 --source-name-bytes 4000" "--watch-stream 1"

echo "== checking =="
"$MV_SCRIPT_DIR/attack-report.py" check "$WORK"
