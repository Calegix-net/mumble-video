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
