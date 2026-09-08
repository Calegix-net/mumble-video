# Movie-night bug fixes — 2026-09-06

Follows the [Linux capture fixes](video-linux-fixes-2026-09-05.md). Five reports
from a real "movie night" session on `mumble-video`, and what was done about each.

## 1. "Click to watch" was not clickable

The placeholder tile said *Click to watch* but only the small eyeball in the hover
bar at the bottom right, or a double-click, actually started watching.

- A single click anywhere on a placeholder now starts watching
  (`VideoGrid::mousePressEvent/mouseReleaseEvent`). The eyeball is drawn large and
  centred as the button it now is; the hover-bar eyeball still works and is what
  stops watching.
- The second half of a double-click on a placeholder is swallowed, so it cannot
  fullscreen whatever tile reflowed into the slot after the first click.
- A single click on a tile that is already showing a picture does nothing, as
  before: fullscreen stays on double-click and on the bar's button.

## 2. Losing track of whose stream is on, and who sees it

Video visibility was permission-scoped only: on a default server everyone may
Enter everywhere, so every stream on the server was announced to every user on it
regardless of channel. Channel moves then never re-evaluated anything.

- **Video now follows the channel, as voice does.** `Server::mayReceiveVideo`
  additionally requires the subscriber to be in the sender's channel or one linked
  to it. The router re-checks this per packet, so relaying stops the moment either
  party leaves.
- **Every channel move is diffed** (`Server::syncVideoVisibility`, from
  `userEnterChannel`). The mover is told about streams now visible to them and sent
  `VideoState(active=false)` for streams no longer visible; everyone who gained or
  lost sight of the mover's own streams is told the same about those. Previously
  only "the mover sees existing streams" was handled: a sharer walking into a room
  was invisible to the people already there, and an unwatched placeholder (which
  holds no subscription for revalidation to drop) survived forever on the clients
  left behind.
- **End-of-stream is no longer rate-limited away.** `msgVideoState` only meters
  `active=true`. A dropped `active=false` used to leave the stream announced,
  replayed to every later joiner and kept on every client until the sender
  restarted. Ending a stream nobody was told about is not relayed, so this is not
  an amplification path.
- **Replaced streams withdraw their old subscription.** When the grid replaces a
  sender's stream of the same kind (restart or resize whose end never arrived) it
  now emits `watchToggled(old, false)` before `watchToggled(new, true)`, so the
  server stops relaying a stream nothing is painting and the per-user subscription
  cap is not leaked into.

## 3. Black patches on screen shares

Screen share always uses TiledImage (JPEG tiles). The receiver's canvas starts
black and only tiles that arrive are painted over it, so every lost tile is a
literal black rectangle until the periodic refresh (up to 2 s). Tiles were lost
because:

- **Bursts.** A keyframe, resize, or a whole screen changing at once emitted every
  tile in one loop - 135 units for 1080p, 510 for 4K, several MB into a UDP
  socket in a few milliseconds - and the encoder recorded each as sent regardless.
  `TiledImageEncoder::MAX_UNITS_PER_FRAME` (128) now caps one call's output; the
  rest is left unhashed and carried over to the next call, walking the grid from
  where it stopped. Nothing is lost, a fully changing screen degrades to a lower
  frame rate instead.
- **Reassembly eviction.** `MAX_PENDING_VIDEO_UNITS_PER_SENDER` was 160, sized for
  1080p; a 4K refresh evicted its own oldest half-received tiles. Now 512, with the
  byte budget raised to 16 MB.
- **Late tiles.** The receiver dropped any tile from frame N arriving after any
  tile of frame N+1, surface-wide. Since only changed tiles are sent, N's
  stragglers usually cover different regions and were thrown away for nothing. The
  watermark is now per tile position (`Surface::tileFrameNumbers`).
- **Socket buffers.** Client and server UDP sockets request 4 MB send/receive
  buffers (best effort; Linux caps at `net.core.rmem_max`).
- **PipeWire empty chunks** produced a genuine black frame, which reached every
  viewer as a flash to black and forced a full-tile burst on the next real frame.
  They are now skipped and the previous frame stays current.

Still open: the transport has no pacing below one encode() call, no NACK and no
FEC. Pacing datagrams across the frame interval in `ServerHandler::sendVideoUnit`
would be the next step if black patches persist on lossy links.

## 4. Fullscreen made the machine crawl

`FullscreenVideoWindow` kept its own `QImage` handle on the focused surface's
canvas. `QImage` is implicitly shared, so the next tile painted into the canvas
detached it: a deep copy of the whole canvas, up to 3840x2160 (about 30 MB), for
every tile - dozens a second on a busy share, about a gigabyte of memcpy a second.

- The window no longer holds an image. It reads the focused picture from the grid
  at paint time through a provider callback; no second owner survives the paint.
- Repaints are coalesced to 33 ms and confined to the monitor region the changed
  tiles map to, instead of re-scaling the whole desktop onto the whole monitor per
  tile.
- Grid tile updates repaint only their own cell.

`TestVideoGrid::fullscreenDoesNotCopyTheCanvasOnEveryTile` pins the canvas's
pixel buffer across a tile arriving while fullscreen.

## 5. No audio for screen shares

What exists: `ScreenAudioBroadcaster` (Opus over the video transport as a second
stream tagged `OpusAudio`), `AudioOutputScreenShare` on the receive side, and a
WASAPI loopback capture on Windows only. The receive path had a real bug:

- `AudioOutput::addFrameToBuffer` looked the sender's voice buffer up with
  `QMultiHash::value()`, which returns the most recently inserted value - the
  screen-share buffer whenever one exists. The cast to `AudioOutputSpeech` failed,
  a new speech buffer was created, and `replace()` overwrote the screen-share entry
  with it. **The share's audio dropped out of the mixer the first time its sender
  spoke**, and the buffer leaked. The lookup now walks all buffers under the sender,
  and `insert()` is used instead of `replace()`. `removeUser` removes every buffer
  under the user, not just one.

What remains to be designed and built for audio to work end to end:

- Linux capture: there is no `AudioLoopbackSource` for PipeWire or a PulseAudio
  monitor, and the PipeWire arm of `toggleScreenShare` never starts the broadcaster.
- A UI toggle and a persisted setting outside Windows; today the "include audio"
  choice lives only in the Windows-only `ScreenSharePickerDialog`.
- A mixer-frequency-zero attach (`AudioOutputScreenShare(0)`) skips resampling
  permanently; the resampler should be created when the mixer is ready.
- Audible playback on Windows has never been verified end to end; only "Opus units
  received" has.

## Validation

Debug build on Fedora 45 (Qt 6.11.1, `-Dice=OFF`, `-Dtests=ON`). Client, server
and every video test target compile clean. Suites run with
`QT_QPA_PLATFORM=offscreen`:

| Suite | Result |
|---|---|
| TestVideoGrid | 41 passed (4 new) |
| TestVideoPipeline | 16 passed (1 new) |
| TestVideoBroadcaster | 16 passed |
| TestVideoFragmentation | 34 passed |
| TestVideoRouter | 18 passed |
| TestVideoTransport | 18 passed |
| TestVideoUi | 10 passed |
| TestVideoCall | 8 passed |
| TestLinuxScreenCapture | 11 passed (needs `dbus-run-session`) |
| TestAudioReceiverBuffer | 10 passed |

Not verified on a live session: the channel-move visibility diff has no
`Server`-level test harness (only `VideoRouter` is unit-tested), and the socket
buffer sizes are best-effort against the kernel cap. Both need a two-client run.
