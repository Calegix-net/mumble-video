# Mock video sources for headless hosts

Battle boxes, containers and CI have no camera and nothing a desktop portal could capture. Two
environment variables let the client stream anyway, so the whole pipeline - encode, fragment, relay,
reassemble, paint - can be exercised with several clients on hosts that have no hardware at all.
Both are read only at share time and only when the variable is non-empty; production builds with
the variables unset behave exactly as before.

| Variable | Effect |
|---|---|
| `MUMBLE_MOCK_CAMERA=1` | *Share Camera* streams a 640x360 synthetic test pattern (`SyntheticVideoSource`) instead of a V4L2 device. |
| `MUMBLE_MOCK_CAMERA_FALLBACK=1` | Same, but only when no real camera is found. |
| `MUMBLE_MOCK_SCREEN=1` | *Share Screen* streams a synthetic 1920x1080 desktop instead of asking the portal. The *Share Screen* control is shown even when no portal is on the session bus. |
| `MUMBLE_MOCK_SCREEN_SIZE=WxH` | Size of the mock desktop, 64x64 up to 7680x4320. |

The mock desktop is drawn by `SyntheticVideoSource::setScreenLike(true)`: a static wallpaper and
bottom bar, a terminal window whose text scrolls one line every four frames, a block bouncing in the
right third that moves every frame, a frame counter in the corner and a clock in the bar. Most tiles
stay byte-identical between frames and a few always change, which is the shape of traffic the tiled
codec is built for. The frame counter lets a viewer's screenshot say exactly which frame it shows, so
a stall, a black patch or a late tile can be matched against the sender's log.

The mock screen is announced as a `Display` source with the `TiledImage` codec, exactly like a
portal capture, so viewers cannot tell the difference and neither can the server.

Example, one headless client:

```
MUMBLE_MOCK_CAMERA=1 MUMBLE_MOCK_SCREEN=1 MUMBLE_MOCK_SCREEN_SIZE=1920x1080 \
QT_QPA_PLATFORM=xcb DISPLAY=:19 flatpak run --user com.calegix.MumbleVideo "mumble://mv1@calegix.com:64738/"
```
