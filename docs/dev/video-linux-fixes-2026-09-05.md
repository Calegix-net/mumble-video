# Linux capture fixes and direct downloads — 2026-09-05

This continues the [client/server review](video-review-2026-09-05.md) and
[Windows capture fixes](video-fixes-2026-09-05.md) on `fix/video-review`.

## Linux changes

- Cancelling screen sharing closes the pending portal Request and Session,
  disconnects response listeners, and ignores late replies from cancelled requests.
  Desktop-initiated session closure now stops capture and clears the local state.
- Source and cursor selections are intersected with the portal's advertised modes.
  Monitor-only portals no longer receive a request for unsupported window capture.
- The server announcement waits for the first captured frame. Time spent choosing
  a window no longer consumes the server's silent-stream timeout.
- PipeWire core round trips report healthy idle capture only while the stream is
  streaming and has produced a frame. Successful replies drive the broadcaster's
  existing refresh path. A disconnected stream or failed core ends capture.
- Stop/restart resets negotiated geometry, first-frame watchdog, drop counters,
  pending images, and health state. Generation checks discard callbacks queued by
  an earlier run. Frame delivery coalesces notifications as well as pixel data.
- Buffer conversion uses wide bounds arithmetic, accounts for chunk offsets and
  row padding, rejects unsupported/corrupt buffers, and respects empty-frame flags.
  It checks image allocation before writing pixels. Format renegotiation cannot
  retain the previous format accidentally.

## Validation

`TestLinuxScreenCapture` exercises pixel conversion and invalid bounds, restart
state, stale callbacks, disconnected streams, and an isolated D-Bus portal process.
The portal fixture sends immediate responses, advertises monitor-only capture,
leaves its picker pending, accepts cancellation, emits a late cancelled response,
and revokes sessions.

All 33 native CTest suites passed (57.16 seconds), including the real TLS/UDP
server integration suite and 11 QtTest entries in the Linux capture suite.
See [native test results](video-review-evidence/linux-ctest.txt).
After the final heartbeat timeout change, the Linux capture, broadcaster, and
pipeline suites passed again (5.30 seconds). The Fedora 42 Release build also passed all 33 suites (43.93 seconds). After
compiler-warning cleanup, the affected release suites passed again (3.11 seconds).
Both the Linux and Windows client builds completed successfully.
The host GNOME portal advertises source and cursor mask 7. The automated portal
fixture validates the protocol; it is not a live compositor capture test.
Linux screen-share audio capture is not implemented. Physical cameras and a full
matrix of Wayland compositors still require device testing.

## Downloads

The [download page](https://calegix.com/dl/mumble/) hosts Windows and Linux ZIPs
under dated paths with SHA-256 checksums. Linux requires x86-64 and glibc 2.41+.
`dev/downloads/index.html` is the source of this static page. Existing historical
artifacts remain at their existing paths.

The site serves files through its existing `mumble-dl` nginx container and
`/dl/mumble/` Traefik route. Both public ZIP URLs passed HTTP 206 byte-range
checks, matching local bytes and file lengths. The public page and checksum file
match the staged versions.

The packaged Linux client also started on the host Wayland desktop with an
isolated, muted profile. This confirms startup and runtime loading, not live
compositor capture.

## Production server

At the user's request, the production `mumble-fork-server` on calegix.com was
upgraded to `localhost/mumble-video-server:3a839ac20`. Its binary SHA-256 is
`885c38c74a9d7a8320e88160829920fb9227600422ea7e61a1594241320f9ce8`, matching
the tested Fedora 42 build. A temporary loopback server passed the TLS/Version
probe before the production replacement. The public server then passed the same
probe on TCP 64738 and retained its existing certificate. The public UDP 64738
endpoint also returned a valid nonce-matched status response (version 1.7.0).
Host networking and
`unless-stopped` restart policy are preserved.

The stopped previous container is `mumble-fork-server-before-3a839ac20`.
A consistent data backup was taken after stopping the old server at
`/home/natej/mumble-fork/server-data-before-3a839ac20`. The active data directory
remains `/home/natej/mumble-fork/server-data`. The temporary preflight server was
removed after verification. No live multi-person call was used as an upgrade check.

## API references

- [ScreenCast portal](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.ScreenCast.html)
- [PipeWire core sync/done protocol](https://docs.pipewire.org/group__pw__core.html)
- SPA buffer bounds follow `spa/buffer/buffer.h` from the installed PipeWire headers.
