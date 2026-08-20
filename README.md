# Mumble Video

A fork of [Mumble](https://github.com/mumble-voip/mumble) that adds camera sharing to the
low-latency voice chat Mumble is known for.

**This is not the Mumble project**, and it is not affiliated with or endorsed by it. "Mumble" is
their name and their work; everything good about the audio path here is theirs. This fork exists
because video kept being requested upstream and never fit their scope.

## What it adds

- **Camera sharing** - a toolbar button next to mute/deafen, a first-run video wizard that measures
  what bitrates actually cost on your camera, and a video panel that appears when somebody shares.
- **Screen sharing** - on Windows, a display (DXGI Desktop Duplication) or a single window
  (Windows.Graphics.Capture), with system or per-application audio over WASAPI loopback. On Linux,
  through the XDG desktop portal and PipeWire - the compositor's own consent dialog decides what is
  shared, which is the only way capture can work on Wayland at all.
- **A dedicated video transport** - video runs on its own UDP channel with its own encryption state,
  its own sequence space and its own MTU, so video loss cannot degrade voice.
- **Two codecs** - VP8 for cameras (roughly 0.5 Mbit/s at 480p), and tiled JPEG for screen content,
  where unchanged regions cost nothing to send.
- **Permissioned routing** - two new ACL privileges, `ShareVideo` and `ReceiveVideo`, enforced by
  the server on every packet delivered, not just at subscribe time. Video is subscribe-on-demand
  rather than broadcast.
- **Keyframe-on-demand** - new subscribers get a keyframe immediately instead of waiting for the
  next scheduled one; stuck decoders re-request. Rate-limited server-side.

## The one caveat that matters

This is a **clean-break fork**: the UDP framing carries an extra media-channel byte, so this client
cannot talk to a stock Mumble server and stock clients cannot talk to this server. Run both halves.

The video transport contains new cryptographic code (sequence-derived nonces, sliding replay window,
keys derived from the TLS session). The construction is conventional and tested, but it has had
**no independent security review**. Treat it accordingly.

## Downloads

Grab a [release](https://github.com/Calegix-net/mumble-video/releases): each carries a Linux bundle,
a flatpak, a Windows wizard installer (`mumble-video-setup.exe`), an MSI for deployment tooling, a
portable Windows zip, and checksums. `dev-latest` is a rolling prerelease rebuilt from every green
push to main; versioned tags are the stable line. Every Linux bundle contains `BUILD-INFO.txt`
naming the exact commit it was built from.

## Known issues

- Received VP8 video can show green or garbled frames after packet loss, until the next keyframe.
  Under investigation - the decoder recovers, but slower than it should.
- Both screen-sharing capture paths are young: they build and pass CI, but have had little time on
  real desktops. Portal/PipeWire behavior varies by compositor; reports welcome.
- No congestion control yet: video is sent at the configured bitrate regardless of what the path can
  carry. The protocol reserves the feedback fields; the sender-side ramp is not written.

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
  'bash /src/scripts/bundle-windows.sh /build /out/bundle && bash /src/scripts/build-msi.sh /out/bundle /out/mumble-video-x64.msi'
```

The test suite (30 suites, including an integration test that spawns a real server and completes a
two-client video call over the wire) runs with `ctest` in the build directory.

## Where the video code lives

| | |
|---|---|
| `src/VideoTransport.*` | per-stream crypt state, nonce derivation, replay window |
| `src/VideoFragmentation.*` | fragment/reassemble units across 1200-byte datagrams |
| `src/murmur/VideoRouter.*` | subscription routing and permission enforcement |
| `src/mumble/VideoGrid.*` | the panel: per-sender surfaces, decoding, layout |
| `src/mumble/VideoBroadcaster.*` | capture-to-encoder pipeline |
| `src/mumble/VP8Codec.*` | libvpx encode/decode |
| `src/mumble/VideoWizard.*` | first-run setup with measured bitrates |

## License

Same as upstream: BSD-style, see [LICENSE](LICENSE). Upstream's full documentation, contribution
guidelines and source-code introduction remain in the tree and largely apply here too.
