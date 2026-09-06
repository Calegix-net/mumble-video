# Linux capture stops and thread errors — 2026-09-06

A reported compositor error is emitted after 30 consecutive buffers fail pixel
conversion. The same path incorrectly counted zero-length metadata/cursor updates
as invalid frames. Such buffers now leave the previous image and first-frame
watchdog unchanged and do not count as invalid frames. Explicit SPA EMPTY frames
produce black pixels even without backing storage; CORRUPTED still takes precedence.

Unreadable buffers now log geometry, format, memory type, storage presence, chunk
size, offset, stride and flags on the first and final drop. These diagnostics do
not log screen pixels. Thread-loop creation and thread startup failures are now
distinguished, and report PipeWire's underlying error before teardown can change it.
The previous generic message alone cannot identify the cause on another user's PC.

All 14 native Linux capture tests passed, including 100 metadata-only buffers
before and after a valid frame, storage-free neutral frames, corrupted chunks,
bounds checks, repeated audio loop creation and portal lifecycle tests. The Fedora
42 release client rebuilt, and capture/broadcaster/pipeline suites passed (3.17 s).
The reported live buffer type and the remote user's thread errno have not yet been
captured, so neither live root cause is claimed as confirmed by these unit tests.

References:
- [OBS PipeWire capture](https://github.com/obsproject/obs-studio/blob/master/plugins/linux-pipewire/pipewire.c) skips zero-size pixel chunks and handles metadata separately.
- [PipeWire thread-loop implementation](https://github.com/PipeWire/pipewire/blob/master/src/pipewire/thread-loop.c) preserves errors from loop creation and thread startup.
- [SPA chunk flags](https://docs.pipewire.org/structspa__chunk.html) distinguish empty neutral media from corruption.
