# Video control hover crash — 2026-09-05

The GitHub development Flatpak built from `e26172e35` crashed in the GUI thread.
The matching runtime resolves its repeating stack as:

```
VideoGrid::enterEvent -> VideoGrid::updateHoveredBar
QWidgetPrivate::setVisible -> QApplicationPrivate::sendSyntheticEnterLeave
QApplicationPrivate::sendMouseEvent -> QWidget::event
VideoGrid::enterEvent -> ...
```

The executable offsets `0x362391` and `0x361a30` map by disassembly to
`enterEvent` and the control-bar hiding pass. The stack pointer at the fault was
at a page boundary. This is a separate failure from the Linux ZIP's mixed
PipeWire libraries: it occurs in the video controls shared by all platforms.
Another caller disconnected within a second, but their crash dump is not available
and their root cause is not confirmed. The server remained running with no restarts.

The hover update now selects the target bar before changing visibility, leaves
an already visible target alone, and guards synchronous re-entry during child
visibility changes. Tests cover repeated enter events and an enter event generated
while hiding a child. Both tests fail against the old implementation and pass with
the fix. The complete native VideoGrid suite passes. The Fedora 42 release client
rebuilt and the Linux capture, VideoGrid, and real client/server call suites all
passed (55.11 seconds).

The installed standard Mumble app (`info.mumble.Mumble`, version 1.5.915) is distinct
from Mumble Video (`com.calegix.MumbleVideo`). Both desktop entries previously used
the label Mumble, which made it easy to launch the wrong one.

## Reveal follow-up

The caller clarified that clicking Reveal on the receiving client crashed the
screen-sharing sender. Reveal sends a keyframe request through the server to the
sender's broadcaster. This does not establish that the sender hit the same hover
failure as the captured local dump.

Additional tests exercise the actual Watch button while a local screen preview
is active, first-frame decoding, hover transitions, and repeated unwatch/re-watch
for both tiled images and VP8. Sender tests issue 20 new-viewer keyframe requests
per codec against an unchanged capture, checking that full frames are emitted and
the broadcaster stays active. Both complete native suites pass (16.07 seconds).
These tests use synthetic capture and do not reproduce the caller's crash. His
exact build revision and crash trace are still needed to identify its cause.
