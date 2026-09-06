# Follow-up fixes — 2026-09-05

Changes are in the local `fix/video-review` worktree based on `a5b462b3a`.
They follow the [Windows capture review](video-review-windows-2026-09-05.md).

## Changes

- Idle capture polls now report successful inactivity separately from failure.
  The broadcaster caches the latest valid image, immediately answers recovery
  requests while capture is healthy, and refreshes quiet streams every two
  seconds. Receiver requests do not extend the capture-health deadline. Low-rate
  sources delivering unchanged frames receive the same time-based refresh.
- Window capture uses a free-threaded frame pool and reads each frame's content
  size. Resizing recreates buffers and restarts capture to obtain a full image
  even when the newly resized window is static. Only valid content is copied.
  Frame, pool, and session lifetimes are explicitly closed.
- Capture size changes allocate a fresh stream ID from the shared allocator,
  reset the encoder, announce the replacement, and end the old stream. Viewers
  preserve their Watch choice while discarding old pixels and decode jobs.
- Process-loopback audio callbacks expose IAgileObject and aggregate the COM
  free-threaded marshaler. Activation failures log their HRESULT; capture-read
  failures end the source instead of leaving it apparently active.
- Settings loading catches JSON schema errors as well as syntax errors, decodes
  transactionally, and uses the existing backup fallback. A missing version field
  or wrong value type no longer leaves half-applied settings or escapes loading.
- Display capture applies monitor rotation and composites separate color,
  monochrome, and masked-color cursor shapes with clipping at display edges.
- Late JPEG units cannot overwrite a newer decoded network frame. This uses a
  conservative frame watermark: older units are discarded even if they cover
  another region; periodic refresh repairs gaps. Same-frame tiles remain allowed.

## Validation

The complete Linux client/server build succeeded. All 32 CTest suites passed,
including the real TLS/UDP server integration tests. After the final low-rate
refresh change, the affected broadcaster and pipeline suites passed again.
New tests cover schema fallback, partial-load rollback, idle and stalled capture,
resize stream IDs, preservation of Watch choices, stale network tiles, rotations,
and all three cursor formats.

The Windows cross-build environment was built locally from
`docker/Dockerfile.windows`. The production capture sources are exercised by the
[Windows device regression program](../../dev/windows-capture-smoke/README.md).
On Windows 11 build 26200 with the QXL driver, the production-source device
program passed all checks with zero failures. Captures grew from 322×272 to
642×452 and shrank to 202×152; saved images show all three colored content edges.
Display capture returned pixels. Selected-process audio delivered 17 non-silent
PCM packets (59,976 bytes); the system-audio exclusion route also activated.
See the [device log](video-review-evidence/fixed-windows-capture.txt),
[grown capture](video-review-evidence/fixed-capture-grown.png), and
[shrunk capture](video-review-evidence/fixed-capture-shrunk.png).

A full patched Windows client was also built and connected to the local patched
server. A separate Linux receiver decrypted and reconstructed tiled window video
and received Opus screen-audio units. Maximizing the shared terminal replaced
stream 2 with stream 4 and changed the remote canvas from 938×628 to 932×887;
the old stream was explicitly ended. The quiet share remained active for over a
minute, and a newly connected receiver reconstructed the current image.
See the [relay log](video-review-evidence/fixed-remote-receiver.txt),
[late-viewer log](video-review-evidence/fixed-late-receiver.txt), and
[remote resized image](video-review-evidence/fixed-remote-after-resize.png).
Audio relay was verified as received Opus units; audible remote playback was not
assessed. The standalone device test separately verified non-silent captured PCM.

The portable [Windows test bundle](https://calegix.com/dl/mumble/2026-09-05/mumble-video-windows-x86_64.zip)
contains the patched client and matching runtime DLLs. The final published archive
and its checksum are listed on the [download page](https://calegix.com/dl/mumble/).

Portrait-display orientation and separate cursor formats have pixel-level unit
tests; physical portrait hardware was not available. Cameras and desktop portals
were not exercised in this Windows run. An earlier released binary's unexplained
Qt6Core access violation was not reproduced or attributed by these fixes.

The temporary client, server, receiver, and HTTP collector were stopped, and
test media was ejected. The Windows VM remains running.

Follow-up publication and Linux results are recorded in the
[Linux fixes report](video-linux-fixes-2026-09-05.md).
