# Mock video sources for headless hosts

Battle boxes, containers and CI have no camera and nothing a desktop portal could capture. A few
environment variables let the client stream anyway, so the whole pipeline - encode, fragment, relay,
reassemble, paint - can be exercised with several clients on hosts that have no hardware at all.
Each one counts as set only when it is non-empty; production builds with the variables unset behave
exactly as before.

| Variable | Effect |
|---|---|
| `MUMBLE_MOCK_CAMERA=1` | *Share Camera* streams a 640x360 synthetic picture (`SyntheticVideoSource`) instead of a real camera, whether or not one is present. It is noise over the whole frame, new every frame, like a sensor's. Encoded in VP8 like every camera, at the camera's configured bitrate and frame rate. |
| `MUMBLE_MOCK_CAMERA_FALLBACK=1` | Same, but only when no real camera is found. |
| `MUMBLE_MOCK_SCREEN=1` | *Share Screen* streams a synthetic 1920x1080 desktop instead of capturing one; there is no picker. The *Share Screen* control is shown even when neither a ScreenCast portal nor an X11 display can be captured. Any non-empty value other than `motion` means this. |
| `MUMBLE_MOCK_SCREEN=motion` | As above, but film-like footage (a drifting gradient, a panning texture, moving shapes): every pixel changes every frame, as in a film or a game, the content the screen-share codec choice is about. |
| `MUMBLE_MOCK_SCREEN_SIZE=WxH` | Size of the mock desktop, 64x64 up to 7680x4320. Anything else is ignored and 1920x1080 used. |
| `MUMBLE_MOCK_SCREEN_AUDIO_APP=name` | The mock screen share's audio comes from this application only, instead of everything the machine is playing except Mumble. See below. |
| `MUMBLE_H264_SOFTWARE=1` | Lets the software x264 encoder stand in when no hardware H.264 encoder opens, so a host with no graphics card can still test H.264 screen shares. Needs an FFmpeg build with libx264. |
| `MUMBLE_VIDEO_STATS=1` | Logs measurements every 2 seconds: `VideoStats tx` per broadcaster, `VideoStats rx` per watched stream, `VideoNackStats` for retransmission requests, `ScreenAudioStats` for received share audio, and `GuiLag`, how late a 10 ms timer fires on the GUI thread. |
| `MUMBLE_VIDEO_UNTHREADED=1` | Encodes and decodes video on the GUI thread instead of on threads of their own, as the tests do. For comparing against the threaded pipeline. |
| `MUMBLE_PICKER_SNAPSHOT=path` | Windows only. Renders the share picker off screen, saves its two tabs as `path-screens.png` and `path-applications.png`, and quits. |

The mock desktop for `MUMBLE_MOCK_SCREEN=1` is drawn by `SyntheticVideoSource::setScreenLike(true)`:
a static wallpaper and bottom bar, a terminal window whose text scrolls one line every four frames, a
block bouncing in the right third that moves every frame, a frame counter in the corner and a clock in
the bar. Most of the picture stays byte-identical between frames and a few regions always change,
which is what a real desktop looks like. The frame counter lets a viewer's screenshot say exactly
which frame it shows, so a stall, a black patch or a late frame can be matched against the sender's
log. `motion` uses `SyntheticVideoSource::setFilmLike(true)` instead.

The mock screen is announced as a `Display` source and goes through the same codec choice as a real
share (`screenShareCodec()` in `MainWindow.cpp`): H.264 when a hardware encoder opens and
`video.screen_share_codec` allows it, VP9 otherwise, at the screen share's bitrate and frame rate.
Viewers cannot tell it from a captured screen and neither can the server.

The mock share also carries audio, captured for real, so share audio can be tested end to end on a
headless host (Linux only: it goes through `PulseLoopbackSource`, which works against PulseAudio and
PipeWire alike). With `MUMBLE_MOCK_SCREEN_AUDIO_APP` unset that is everything the machine is playing
except Mumble, as a real share's default. Set, it is only the playback streams whose
`application.process.binary` - or, for a stream without one, `application.name` - equals the value,
which is what the share picker's "audio from" choice does. If no audio server is reachable the
picture is shared without it.

The rig in `mumla-video/rig` uses this to keep the test machine's own audio out of the share:
`share-audio-tone.sh` plays a looping tone into a private null sink as `mv-rig-tone`, and
`start-sharer.sh` starts the sharer with `MUMBLE_MOCK_SCREEN_AUDIO_APP=mv-rig-tone`, so the share
announces an `OpusAudio` stream next to the picture.

Example, one headless client:

```
MUMBLE_MOCK_CAMERA=1 MUMBLE_MOCK_SCREEN=1 MUMBLE_MOCK_SCREEN_SIZE=1920x1080 \
QT_QPA_PLATFORM=xcb DISPLAY=:19 flatpak run --user com.calegix.MumbleVideo "mumble://mv1@calegix.com:64738/"
```

A playback stream to stand in for an application's audio, and a sharer that sends only that:

```
pw-play -P '{ application.name=mv-rig-tone application.process.binary=mv-rig-tone }' tone.wav &
MUMBLE_MOCK_SCREEN=motion MUMBLE_MOCK_SCREEN_AUDIO_APP=mv-rig-tone MUMBLE_VIDEO_STATS=1 \
QT_QPA_PLATFORM=xcb DISPLAY=:19 flatpak run --user com.calegix.MumbleVideo "mumble://mv1@calegix.com:64738/"
```
