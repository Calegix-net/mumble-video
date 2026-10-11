# Mumble Video

A fork of [Mumble](https://github.com/mumble-voip/mumble) that adds camera and screen sharing to the
low-latency voice chat Mumble is known for.

**This is not the Mumble project**, and it is not affiliated with or endorsed by it. "Mumble" is
their name and their work; everything good about the audio path here is theirs. This fork exists
because video kept being requested upstream and never fit their scope.

## What it adds

- **Camera sharing** - a toolbar button beside mute, deafen and record, a first-run video wizard (when
  a camera is found) that measures what bitrates actually cost on your camera, and a video panel that
  appears when somebody shares. Watching is opt-in: click a tile to start.
- **Screen sharing** - a whole screen or a single window, with or without audio. On Windows the
  picker shows live thumbnails; displays go through DXGI Desktop Duplication and windows through
  Windows.Graphics.Capture. On Linux, Mumble Video asks screen or window and which audio, then the XDG
  desktop portal's own dialog (with its own previews) decides what is shared, and frames arrive over
  PipeWire. That is the only way capture works on Wayland.
- **Share audio** - everything the computer is playing except Mumble itself, or just one
  application: WASAPI process loopback on Windows, per-application PulseAudio/PipeWire capture on
  Linux. Only people watching a share hear it. Each viewer has a 0-200% volume and mute control on
  the tile and in fullscreen.
- **Who is watching** - your own tile shows how many people are watching your share, and lists them
  with the volume each one hears it at.
- **Codecs** - VP8 for cameras. For screens, hardware H.264 when the graphics card has an encoder
  (NVENC, AMF or Media Foundation on Windows; VA-API or NVENC on Linux), otherwise VP9 in
  screen-content mode. Defaults are 6 Mbit/s for H.264 and 3 Mbit/s for VP9; Configure > Video can
  change both or force VP9. A still screen stops sending after a second. Encoding and decoding
  run on their own threads.
- **A separate video transport** - video shares voice's UDP socket, marked by a channel byte, but has
  its own encryption state, its own sequence space and its own 1200-byte datagrams, so a lost or
  replayed video packet never touches voice's crypt state. Heavy video can still fill the socket
  buffer that voice uses too; see the UDP buffer advice below.
- **Loss recovery** - a viewer asks the sender to resend the fragments or frames it missed
  (`VideoNack`) and holds later frames briefly while they arrive, falling back to a keyframe request
  only if they don't. Share audio has a jitter
  buffer, Opus FEC and loss concealment. Shares and watching survive an automatic reconnect.
- **Permissioned routing** - two new ACL privileges, `ShareVideo` and `ReceiveVideo`, enforced by
  the server on every packet delivered, not just at subscribe time. Video is subscribe-on-demand
  rather than broadcast.
- **Keyframe-on-demand** - new subscribers get a keyframe immediately instead of waiting for the
  next scheduled one; stuck decoders re-request. Rate-limited server-side.

## The one caveat that matters

Every UDP datagram carries an extra media-channel byte, so over UDP this client and a stock Mumble
server do not understand each other, and neither do a stock client and this server. Mixing them is
untested and unsupported, and video needs both halves. Run this client with this server.

The video transport contains new cryptographic code: the video key and nonce bases are derived with
SHA-256 from the voice channel's session key, nonces come from the sequence number, and a sliding
window rejects replays. The construction is conventional and tested, but it has had **no independent
security review**. Neither has the code that parses other people's media: the FFmpeg H.264 decoder,
VP8/VP9 and tile (JPEG) decoding, share-audio Opus decoding, and fragment reassembly. Treat it
accordingly.

## Downloads

Grab a [release](https://github.com/Calegix-net/mumble-video/releases): each carries a Linux bundle,
a flatpak, a Windows wizard installer (`mumble-video-setup.exe`), an MSI for deployment tooling, a
portable Windows zip, the server binary, and checksums. Versioned tags are the stable line;
`dev-latest` is a rolling prerelease rebuilt from every green push to main. Every Linux bundle
contains `BUILD-INFO.txt` naming the exact commit it was built from. The server is also published as
a container image, `ghcr.io/calegix-net/mumble-video-server`.

Server hosts should raise the kernel's UDP buffer limits, or keyframe bursts overflow the socket
and viewers see black patches:

```sh
sudo sysctl -w net.core.rmem_max=8388608 net.core.wmem_max=8388608
```

## Known issues

- No congestion control yet: video is sent at the configured bitrate regardless of what the path can
  carry. The protocol reserves the feedback fields; the sender-side ramp is not written.
- Portal/PipeWire screen capture behaves differently from one compositor to another; reports welcome.

## Building

Everything builds in containers so the host is never touched:

```sh
# Linux client + server + tests
docker build -t mumble-build -f docker/Dockerfile docker/
docker run --rm -v "$PWD":/src -v "$PWD/build":/build mumble-build bash -c \
  'cmake -S /src -B /build -Dtests=ON -Dice=OFF -Doverlay-xcompile=OFF && cmake --build /build -j$(nproc)'

# Windows client (MinGW cross-compile) + MSI
docker build -t mumble-build-windows -f docker/Dockerfile.windows docker/
docker run --rm -v "$PWD":/src -v "$PWD/build-win":/build mumble-build-windows bash /src/scripts/build-windows.sh
docker run --rm -v "$PWD":/src -v "$PWD/build-win":/build -v "$PWD/out":/out mumble-build-windows bash -c \
  'bash /src/scripts/bundle-windows.sh /build /out/bundle && bash /src/scripts/build-msi.sh /out/bundle /out/mumble-video-x64.msi 1.9.0'

# Flatpak (privileged, because flatpak-builder needs its own namespaces)
docker build -t mumble-flatpak-build -f docker/Dockerfile.flatpak docker/
docker run --rm --privileged -v "$PWD":/src -v "$PWD/flatpak-state":/state -v "$PWD/out":/out \
  mumble-flatpak-build bash /src/scripts/build-flatpak.sh /src /state /out/mumble-video.flatpak
```

The test suite runs with `ctest` in the build directory. It has 35 suites, including an
integration test that spawns a real server and completes a two-client video call over the wire.

For testing without a camera or a desktop, `MUMBLE_MOCK_CAMERA` and `MUMBLE_MOCK_SCREEN` stream
synthetic video, and `mumble rpc sharescreen|sharecamera|watchall|...` drives the client from a
script. See [docs/dev/mock-video-sources.md](docs/dev/mock-video-sources.md).

## Where the video code lives

| | |
|---|---|
| `src/VideoTransport.*` | per-stream crypt state, nonce derivation, replay window |
| `src/VideoFragmentation.*` | fragment/reassemble units across 1200-byte datagrams |
| `src/murmur/VideoRouter.*` | subscription routing and permission enforcement |
| `src/murmur/Server.cpp` | relays video units and resend requests (`relayVideoNack`) |
| `src/mumble/VideoGrid.*` | the panel: per-sender surfaces, decoding, layout |
| `src/mumble/VideoStreamDispatcher.*` | tracks announced streams, hands share-audio units to playback |
| `src/mumble/VideoBroadcaster.*` | capture-to-encoder pipeline |
| `src/mumble/VpxCodec.*`, `H264Codec.*` | libvpx (VP8/VP9) and FFmpeg H.264 encode/decode |
| `src/mumble/ScreenAudioBroadcaster.*` | share audio Opus encoding and sending |
| `src/mumble/PulseLoopbackSource.*`, `Wasapi*LoopbackSource.*` | share audio capture on Linux and Windows |
| `src/mumble/AudioOutputScreenShare.*` | share audio playback: jitter buffer, FEC/PLC, volume |
| `src/mumble/ScreenSharePickerDialog.*`, `LinuxSharePickerDialog.*` | the share pickers |
| `src/mumble/VideoWizard.*` | first-run setup with measured bitrates |

## License

Same as upstream: BSD-style, see [LICENSE](LICENSE). Upstream's full documentation, contribution
guidelines and source-code introduction remain in the tree and largely apply here too.
