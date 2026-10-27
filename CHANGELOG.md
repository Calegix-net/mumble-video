# Changelog

What changed in Mumble Video, newest first, written for the people who use it.

This file starts at 1.10.0. Earlier releases are described on the
[releases page](https://github.com/Calegix-net/mumble-video/releases), which is where the notes for
them were written at the time; repeating them here from commit subjects would only be a worse copy.

Versions follow [semantic versioning](https://semver.org/) loosely: the patch number moves for fixes,
the minor number for features, and the fork keeps its own numbering rather than tracking upstream
Mumble's. "Upstream" below always means the Mumble project this is forked from.

## Unreleased

### Screen sharing

- **Sharper, cheaper VP9 pictures.** The receiver's colour conversion now goes through libswscale with
  accurate rounding instead of a hand-written loop. Measured on the benchmark rig, receiver CPU for a
  1080p30 VP9 share falls from 41–42 % to 24–28 %, and the conversion itself is more accurate than
  what it replaced (48.4 dB against 46.8 dB on a reference frame). Nothing about the picture you send
  changed; this is the cost of displaying one.
- **The screen-share picker's three options are now actually exclusive.** On Linux, "whole screen",
  "a window" and "a region" were three independent buttons: picking one did not unpick the others.
- **"Show my cursor" works.** The Windows capture path accepted the setting and ignored it, so the
  cursor was always sent.
- **A share that stops and starts again tells you whose it was.** The message naming a stale stream
  had no name in it.
- **A server without video support says so, once.** Rather than leaving the video controls greyed out
  with no explanation, the client logs one line per connection and the disabled menu entries explain
  themselves in their tooltips.

### The video window

- **The volume slider follows the pointer.** Pressing anywhere on a stream's volume slider now sets
  the level there and starts dragging, instead of stepping towards the pointer a page at a time. The
  level is also announced to screen readers.
- **The video window has a menu entry.** It could be closed and then only reopened by starting a
  share; `Configure → Video window` toggles it, and it is in the window's own right-click menu with
  the other docks.
- **The Video Wizard is back in the Configure menu**, where it was being removed each time the menu
  opened.
- **Hovering a tile no longer fights with dragging its volume slider.**

### Reliability

- **A stream that never receives its first keyframe now says so in the logs** instead of reporting the
  same thing a healthy stream reports.
- **A replacement stream resets the H.264 decoder**, rather than feeding a new stream's frames to a
  decoder still holding the old one's reference frames.
- **Frames claiming to start somewhere other than the origin are refused** on the VP8 and VP9 paths,
  where only one tile per frame is possible by construction.
- **Retransmission attempts are spent only on requests that were sent.** A request that was built and
  then dropped — because the message was full — used up one of the unit's three attempts anyway, so
  under load units gave up after fewer real attempts than intended. The per-second request budget also
  rose from 30 to 60, the old figure having been justified by a limit that does not apply.
- **The UDP buffer warning reports the size the socket actually has**, not the size that was asked
  for; on Linux the kernel reports double what it gives you, so the warning was wrong by a factor of
  two in both the client and the server.
- **A video key that cannot be derived is now logged** by both ends. It meant video was silently off
  for a whole connection with nothing saying why.

### For developers

- **`docs/ARCHITECTURE.md`** — a guided tour from capture to encode to network to decode to render,
  the threading model, and where the video code touches upstream Mumble.
- **`docs/CONTRIBUTING.md`** — how to build, test, benchmark and add a feature.
- **Fuzz targets for the three things that read bytes off the network** — the reassembler, the video
  crypt state and the media demultiplexer — in `src/mumble-core/fuzz`. They build as corpus replayers
  on every build and run under `ctest`; with clang and `-DMUMBLE_CORE_FUZZERS=ON` the same sources
  become libFuzzer fuzzers with address sanitizer.
- **Ten new unit tests** across the slider, the grid, the reassembler's attempt accounting and the
  colour conversion, which now exercises the real libswscale path rather than only the fallback.
- **A build directory is no longer tracked.** 80.8 MB of build output had been committed.
- **Frame dumping for measurement.** With `MUMBLE_VIDEO_DUMP_TX` and `MUMBLE_VIDEO_DUMP_RX` set, both
  ends write the frames they encode and decode as PNGs named by the sender's frame number, so a
  quality metric can compare exactly the right pair.
- **Paint delay is measured and reported** in the video statistics, against a windowed arrival floor
  rather than the capture timestamp, which is not a wall clock and never was.

## 1.10.0 — 2026-10-14

See the [1.10.0 release notes](https://github.com/Calegix-net/mumble-video/releases/tag/v1.10.0).
The commits merged to `main` after that tag and before this changelog existed were: keeping the screen
awake while video is streaming; refusing chat pictures that would exhaust memory; ignoring `mumble://`
links that arrive while quitting; reading certificates saved by an OpenSSL 1.x Mumble; global
shortcuts on Wayland through the desktop portal; moving the flatpak to the KDE 6.11 runtime; making
channel joins by double-click deliberate; dragging a popped-out video window by its picture; showing
who is watching your tiles without hovering; sending screen shares at their configured frame rate;
restarting a running camera when its settings change; region sharing and hiding others' video; and
building the server with Ice.

## Earlier releases

| Version | Date |
| --- | --- |
| [1.9.1](https://github.com/Calegix-net/mumble-video/releases/tag/v1.9.1) | 2026-10-11 |
| [1.9.0](https://github.com/Calegix-net/mumble-video/releases/tag/v1.9.0) | 2026-10-11 |
| [1.8.2](https://github.com/Calegix-net/mumble-video/releases/tag/v1.8.2) | 2026-09-15 |
| [1.8.1](https://github.com/Calegix-net/mumble-video/releases/tag/v1.8.1) | 2026-09-08 |
| [1.8.0](https://github.com/Calegix-net/mumble-video/releases/tag/v1.8.0) | 2026-08-19 |
| [1.7.1](https://github.com/Calegix-net/mumble-video/releases/tag/v1.7.1) | 2026-08-06 |
| [1.7.0](https://github.com/Calegix-net/mumble-video/releases/tag/v1.7.0) | 2026-08-04 |
