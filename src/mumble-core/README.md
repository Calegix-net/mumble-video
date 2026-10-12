# mumble-core

The protocol code of mumble-video with no Qt and no sockets: the wire formats, the UDP crypto, video
fragmentation and reassembly, and the control-stream framing. The desktop client and server link it
through `shared`. Other hosts (the Android app, over JNI) use it on its own.

## Building it on its own

```sh
cmake -S src/mumble-core -B build-core -DCMAKE_BUILD_TYPE=Release
cmake --build build-core
ctest --test-dir build-core
```

It needs OpenSSL (libcrypto) and Protobuf, and nothing else. The options are:

- `MUMBLE_CORE_LEGACY_UDP` (default `ON`) keeps the pre-1.5 UDP format. Leave it off for a client that only
  ever talks to current servers. In the main tree it is always on.
- `MUMBLE_CORE_VERSION` is the version string the core reports.
- `tests` builds `TestMumbleCore`.

CI builds it exactly this way, in a configure that never looks for Qt. That is the check that nothing in
here has started to need Qt.

## For a host

Include `include/mumble-core/MumbleCore.h` and nothing else. The header comment explains how the bytes
flow. In short, the host owns the TLS connection and the UDP socket:

- `ControlReader` and `frameControlMessage()` split the TLS stream into messages and back. The host parses
  the control messages itself from `Mumble.proto`, except CryptSetup, which goes to the session.
- `MediaSession` is one connection's UDP side. Datagrams go in. Audio packets (still Opus-encoded), whole
  video units, pongs, and datagrams and control messages to send come out.

Logging goes through `CoreLog.h`. Point `Mumble::Core::setLogHandler()` at the host's log; until then it
writes to stderr.

## Test vectors

`test-vectors/` holds known-answer vectors as JSON, with every byte in hex:

- the OCB2 primitive
- whole audio and video datagrams
- the video key derivation

`generate.py` there writes them. It is a separate Python implementation, written from the C++ but not
linked to it. `TestMumbleCore` checks the C++ against these files, and another implementation should check
itself against them too.

## Layout

| Files | What |
| --- | --- |
| `Mumble.proto`, `MumbleUDP.proto` | the wire messages; generated code is built here |
| `MumbleProtocol`, `PacketDataStream.h` | UDP audio and ping encoding, protobuf and legacy |
| `crypto/` | OCB2-AES128 (audio), random, `Digest` (SHA-1/256) |
| `VideoTransport` | the video channel's crypt state and datagram layout |
| `VideoFragmentation` | splitting units into datagrams, reassembly, NACK bookkeeping |
| `MediaDemux` | which channel a datagram is on |
| `VideoReceiver` | decrypt, reassemble, and pace retransmission requests |
| `ControlFramer` | the 6-byte control message header |
| `CryptoSession` | both crypt states, keyed from CryptSetup |
| `MumbleCore.cpp` | the public API above |

Still in the desktop client, to come later: keyframe gating and joining H.264 units (VideoGrid), and voice
playback (Opus decode, jitter buffer, loss concealment).
