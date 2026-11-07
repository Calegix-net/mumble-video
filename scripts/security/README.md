# Live security tests

A scriptable, headless Mumble client (`mv-attack`) that connects to a running server exactly as a real
client does, and then does things an ordinary authenticated user could do but should not get away with.
It turns a claim read out of the code into something observed against a live server, and it is the
regression test for the authorization fixes on the video path.

It is a verification tool for a server you run. Every scenario is one authenticated user sending
ordinary, correctly-encrypted traffic; nothing here forges authentication, breaks the cipher, or
reaches any host but the one on the command line. The whole thing runs inside a throwaway network
namespace on an isolated loopback, the same way `scripts/benchmark` does.

## What it checks

`run-attacks.sh` runs five scenarios and `attack-report.py` judges each:

| scenario | what it proves |
|---|---|
| `baseline-audio` | an ordinary OpusAudio share reaches an ordinary watcher &mdash; the control, so a later "0 units" means *blocked*, not *broken* |
| `muted-audio` | a self-muted sender's audio does **not** reach a watcher |
| `deaf-watcher` | a self-deafened watcher receives **no** share audio |
| `muted-picture` | a self-muted sender's **picture** still reaches the watcher &mdash; the mute gates sound only |
| `source-name-truncated` | a 4000-byte `source_name` is stored and relayed capped at 512 bytes |

The `self_mute` and `self_deaf` the attacker sets on itself hit the same `bSelfMute` / `bSelfDeaf`
server checks that an admin's mute and deafen do, so this exercises the real code path without needing
an admin action to set it up.

### The tests have teeth

Run against a server built before the fixes (work/cleanup's tip, `00f3a86c4`), the suite reports:

```
[PASS] baseline-audio         112 units
[FAIL] muted-audio            110 units from a self-muted audio sender (must be 0)
[FAIL] deaf-watcher           112 audio units to a self-deafened watcher (must be 0)
[PASS] muted-picture          112 units
[FAIL] source-name-truncated  4000 bytes (must be <= 512)
```

Against the fixed server, 5/5 pass. Same binary, same scenarios; only the server changed. A test that
cannot fail proves nothing, so the negative control is the point.

## Building

`mv-attack` links `mumble-core`, so the control framing and the OCB2 video crypto are the shipping
implementation rather than a reimplementation that might hide the bug under test. It needs no Qt and no
display.

```sh
cmake -S scripts/security -B build/attack -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/attack
```

## Running

```sh
MV_ATTACK=build/attack/mv-attack \
MV_SERVER=/path/to/mumble-server \
bash scripts/security/run-attacks.sh
```

Needs `unshare` (user namespaces), `openssl` and `python3`. It brings the namespace's loopback up
itself; the one line that, left out, silently makes every client fail to connect.

## What is NOT covered here, and why

- **Forgery and the sequence-jump lockout.** A blind forgery against the three-byte tag is one chance
  in 16.7 million, which is not something to reproduce live against a random per-connection key. That
  belongs to `src/mumble-core/fuzz/MumbleFuzzcrypt`, which has a fixed key and found it. The committed
  corpus entry `corpus/crypt/crypt-forged-sequence-jump` is that finding.
- **Admin mute, and the linked-channel `Speak` gap.** Admin mute goes through the same check this suite
  exercises through self-mute; the linked-channel gap (an open finding) needs a server with a
  configured ACL, which this harness does not yet set up.
- **Amplification and the per-datagram ACL cost.** These are load questions for the benchmark rig, not
  pass/fail questions for here.
