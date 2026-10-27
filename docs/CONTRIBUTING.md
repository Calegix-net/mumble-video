# Contributing to Mumble Video

This is a fork of [Mumble](https://github.com/mumble-voip/mumble) that adds video. Start with
[`README.md`](../README.md), which is an accurate map of what the product does, and
[`ARCHITECTURE.md`](ARCHITECTURE.md), which is a guided tour of the video pipeline. Upstream's own
[`CODING_GUIDELINES.md`](../CODING_GUIDELINES.md) and
[`COMMIT_GUIDELINES.md`](../COMMIT_GUIDELINES.md) still apply and this document does not repeat them.

## Build

Everything builds in containers, so the host is never touched. These are the commands CI runs.

```sh
# Linux client + server + tests
docker build -t mumble-build -f docker/Dockerfile docker/
docker run --rm -v "$PWD":/src -v "$PWD/build":/build mumble-build bash -c \
  'cmake -S /src -B /build -Dtests=ON -Dice=OFF -Doverlay-xcompile=OFF && cmake --build /build -j$(nproc)'

# Windows client (MinGW cross-compile) + installers
docker build -t mumble-build-windows -f docker/Dockerfile.windows docker/
docker run --rm -v "$PWD":/src -v "$PWD/build-win":/build mumble-build-windows bash /src/scripts/build-windows.sh
```

For day-to-day work a native build is faster. Nothing below needs a container.

```sh
# Linux, native
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -Dtests=ON -Dice=OFF -Doverlay=OFF -Doverlay-xcompile=OFF -Dzeroconf=OFF -Dtranslations=OFF
cmake --build build -j"$(nproc)"
```

On **Windows with MSYS2 mingw64**, put `C:/msys64/mingw64/bin` on `PATH` first - it supplies both the
toolchain and the Qt that `mumble.exe` links against. Without it the client starts and exits with no
message, and a test binary prints nothing at all, because the test executables are GUI-subsystem
binaries: pass `-o out.txt,txt` to see their output.

```sh
export PATH="/c/msys64/mingw64/bin:$PATH"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -Dtests=ON -Dlto=OFF -Doverlay=OFF -Dzeroconf=OFF -Dice=OFF -Dtranslations=OFF
cmake --build build -j12
```

If the build fails on `ServerInformation.h:15: error: using typedef-name 'ServerInformation' after
'class'`, that is a collision with a Windows SDK typedef in `combaseapi.h`, which is guarded by
`NTDDI_VERSION >= 0x06020000` and reaches that translation unit because the Windows capture headers
include `<d3d11.h>` and AUTOMOC compiles every `moc_*.cpp` into one file. Adding
`-DCMAKE_CXX_FLAGS="-include <absolute path to a header that lowers _WIN32_WINNT>"` works around it;
the real fix is to stop those headers including `<d3d11.h>`, which is on the backlog.

### CMake options that matter here

| Option | Default | Effect |
|---|---|---|
| `h264` | ON | Hardware H.264 through FFmpeg. Also gives the VP8/VP9 decoder its libswscale colour conversion; without it that falls back to a scalar loop several times slower. |
| `screen-share-pipewire` | ON | Wayland and modern X11 capture, through the XDG desktop portal. |
| `screen-share-x11` | ON | The X11 fallback, for a session with no portal. |
| `tests` | OFF | The ctest suites. Turn it on. |

`docs/dev/build-instructions/cmake_options.md` lists the rest, inherited from upstream.

## Test

```sh
cd build && QT_QPA_PLATFORM=offscreen ctest --output-on-failure
```

`QT_QPA_PLATFORM=offscreen` is what makes the widget suites run without a display. Some of them do
construct real windows, so they need a platform plugin; `offscreen` provides one.

The video suites, and what each is for:

| Suite | Covers |
|---|---|
| `TestVideoGrid` | The receiver: surfaces, frame ordering, holding across a gap, the playout queue, layout, pop-out and fullscreen windows, the hover bars. |
| `TestVideoBroadcaster` | The send path against a synthetic source: encode sizes, resize handling, still-screen suppression. |
| `TestVpxCodec` | VP8 and VP9 encode/decode, including colour fidelity through the round trip. |
| `TestH264Codec` | Access-unit splitting and reassembly. It does **not** open an encoder. |
| `TestVideoFragmentation` | Fragmenting and reassembling units, the pending-unit limits, and retransmission requests. Every parser bound lives here. |
| `TestVideoTransport` | The video crypt state: nonce derivation, the replay window, forgery. |
| `TestVideoRouter` | Server-side subscription routing and permission enforcement. |
| `TestVideoCall` | An integration test: a real server and two clients completing a video call over the wire. |
| `TestVideoUi` | The bandwidth probe, region cropping, and the stream volume slider. |
| `TestVideoPipeline` | Capture to encoder to units, unthreaded. |

Two things to know before adding a test:

- **`VideoGrid` must stay free of the `Global` singleton.** `TestVideoGrid` compiles `VideoGrid.cpp`
  standalone against Qt, libvpx and `shared`, and nothing else. `grep -n 'Global::' src/mumble/VideoGrid.cpp`
  should find nothing. If you need something from the client's state, pass it in or emit a signal.
- **The mock sources are how anything gets tested without hardware.** See
  [`docs/dev/mock-video-sources.md`](dev/mock-video-sources.md): `MUMBLE_MOCK_CAMERA`,
  `MUMBLE_MOCK_SCREEN` (a deterministic desktop with scrolling text, a moving block and a frame
  counter), `MUMBLE_VIDEO_STATS`, `MUMBLE_VIDEO_DUMP_TX`/`_RX`, and `mumble rpc sharescreen|watchall|…`
  to drive a client from a script.

### Fuzz

Four things read bytes a hostile peer chose: the video crypt state, the media demultiplexer, the
reassembler, and `VideoReceiver::receive`, which is all three of those in one call. Each has a fuzz
target in [`src/mumble-core/fuzz`](../src/mumble-core/fuzz), and on an
ordinary build each one is compiled as a replayer that runs its committed corpus under `ctest` — so
every input that has ever broken a parser is re-checked on every build, for milliseconds.

To fuzz for real you need clang:

```sh
cmake -S src/mumble-core -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ -DMUMBLE_CORE_FUZZERS=ON
cmake --build build-fuzz
./build-fuzz/MumbleFuzzreassembly src/mumble-core/fuzz/corpus/reassembly -max_total_time=3600
```

**If you touch a wire format, regenerate the seeds** — they are produced by the real fragmenter, so
stale seeds quietly stop being valid packets and the fuzzer goes back to failing the Protobuf parse a
million times:

```sh
./build/tests/MumbleFuzzMakeSeeds src/mumble-core/fuzz/corpus
```

**If a fuzzer finds something, the input goes in the corpus** next to the seeds, renamed to say what
it is. See [`corpus/README.md`](../src/mumble-core/fuzz/corpus/README.md).

## Benchmark

**Measure before and after. A change to the pipeline that is not measured is not finished.**

The harness runs a real server and two real clients, each on its own X server, inside an unshared
network namespace so that `tc netem` can shape the loopback without root. One client shares the mock
desktop; the other watches it.

It lives in [`scripts/benchmark/`](../scripts/benchmark). `setup-rig.sh` builds everything it needs
from nothing - a server with a database, two client profiles with generated certificates, the mock
desktop - so a contributor who has never seen the machine it was written on can reproduce any number
in the changelog. `env.sh` holds every path and every default, and each one can be overridden from the
environment. Its [README](../scripts/benchmark/README.md) explains why each part is built the way it
is; read that rather than re-deriving it.

```sh
scripts/benchmark/setup-rig.sh                    # once: server + two client profiles
scripts/benchmark/bench.sh <label> <seconds> <auto|vp9> clean loss2 delay100 bw2mbit bad drop
```

Runs land under `$MUMBLE_BENCH_WORK/out/<label>/`, which defaults to
`~/.cache/mumble-video-bench/out`. The rig's working state stays out of the repository because it
holds certificates and a server database.

**A run asserts the commit it built, and you should trust nothing that does not.** One run silently
measured a binary four commits stale - a fetch refspec without a leading `+`, an amended branch, a
refused non-fast-forward fetch, and a checkout that quietly stayed put - and the afternoon spent
investigating the quality regression it reported was spent on a bug that had already been fixed.

Results land in `out/<label>/` as `summary.txt` (a table) and `results.json`. The scenarios are no
impairment, 2% loss, 100 ms each way, a 2 Mbit/s cap, and all three at once.

What it reports, and where each number comes from:

- **From the client's own `MUMBLE_VIDEO_STATS` lines**: frame rate, bitrate, encode time (mean and
  worst), dropped frames, units per second, received frames, the worst gap between paints, keyframe
  requests and why, retransmission requests, abandoned gaps, and `paintDelayMs` - arrival-to-paint
  above the smallest arrival delay seen, which is the receiver's share of glass-to-glass.
- **From `/proc`**: CPU and resident memory per process, sampled every second.
- **From `nvidia-smi`**: GPU and hardware-encoder utilisation.
- **From VMAF, PSNR and SSIM** over frame-matched pairs: the quality pass runs the same scenario again
  with `MUMBLE_VIDEO_DUMP_TX`/`_RX` set, so the frame the encoder was handed and the frame the decoder
  produced are compared by frame number. Performance and quality are separate runs on purpose - saving
  a PNG per frame costs real CPU, so a run that measures CPU must not be dumping frames.

Two more scripts, for the questions a 40-second run cannot answer:

```sh
scripts/benchmark/profile.sh 30 out/profile   # perf, flat and folded stacks for both ends
scripts/benchmark/soak.sh 120 out/soak        # two hours, with a memory slope per process
```

`profile.sh` records frame-pointer call graphs, which means **the build it profiles must be
configured with `-fno-omit-frame-pointer`**, and `RelWithDebInfo` is a good choice for the rest:

```sh
cmake -S . -B build/profile -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_CXX_FLAGS=-fno-omit-frame-pointer -DCMAKE_C_FLAGS=-fno-omit-frame-pointer \
      -Dtests=OFF -Dice=OFF -Doverlay=OFF -Dlto=OFF
MUMBLE_BUILD=$PWD/build/profile scripts/benchmark/profile.sh 30 out/profile
```

DWARF unwinding would avoid that requirement, and `MV_CALL_GRAPH=dwarf` selects it, but only on a host
whose `perf` was built against libunwind or libdw. One that was not - the WSL2 kernel's `perf`, for
instance - accepts the option, records the stack bytes, unwinds none of them, and produces a flat
profile that looks entirely normal beside a call graph that is silently empty. The script warns about
both halves of this now; it did not always, and two profiling runs were lost to it.

Expect most of either profile to be in libraries rather than in `mumble`: on a 1080p30 VP9 share,
roughly a third of the sender is in Qt's image code and another third in libswscale, and nearly half
the receiver is in libavcodec. Those are real findings, and the hexadecimal addresses beside them are
simply what a system library without a debug package looks like.

## Add a feature

1. **Read the pipeline first.** [`ARCHITECTURE.md`](ARCHITECTURE.md) says where each stage lives and
   which thread it runs on. Most mistakes in this code have been thread mistakes.
2. **Decide whether it belongs behind an existing seam.** The video code is deliberately separable from
   upstream Mumble so that merges stay cheap. A new capture backend is a `VideoSource` subclass and
   touches nothing else. A new codec is a pair of classes plus an enumerator in `Mumble.proto`. A
   change that edits an upstream file is the expensive kind; prefer a new file.
3. **Check the server side.** Anything touching what is announced, subscribed or routed has a
   counterpart in `src/murmur/VideoRouter.*` and `src/murmur/Messages.cpp`. State explicitly, in the
   commit message, whether the server needs a change - including when it does not, and why.
4. **Protocol changes are append-only.** `src/mumble-core/Mumble.proto` is proto2 with closed enums:
   enumerator 0 of each video enum is deliberately inert, so a peer that does not know a new value
   drops the stream instead of decoding it as whatever came first. Add fields and enumerators, never
   renumber. `VideoSubscribe` reserves indices for receiver feedback that nothing yet sends.
5. **Test the pure logic.** Packetization, bitrate arithmetic, region conversion, protocol parsing and
   the volume slider's position mapping are all pure functions that were pulled out of their widgets
   and classes specifically so they could be tested. Keep doing that: a function that needs a window
   or a camera to test will not be tested.
6. **Fuzz anything that parses the network.** `VideoCryptState::decrypt`, `MediaDemux::classify`,
   `VideoReassembler::processPacket` and `VideoReceiver::receive` all have targets. The one entry point
   for hostile bytes that does not is **the server's**: `Server::handleVideoNack` and the relayed-video
   parse in `src/murmur/Server.cpp`, which live in the Qt server target rather than in `mumble-core`, so
   a `mumble-core` fuzzer cannot reach them. Giving them one means either lifting the parse into the
   core or building a server-side harness; the first is the better change. See [Fuzz](#fuzz) above.
7. **Benchmark it**, as above, and put the before and after numbers in the commit message.
8. **Say why, not what.** A comment that restates the code is noise; a comment explaining a
   trade-off, a workaround, a driver quirk or a measurement is what makes this code maintainable. Most
   of the comments here are of the second kind and they are the reason the pipeline can be changed at
   all.

## Commit messages

The repository's own history is the style guide. A video commit says what changed, **why** it was
wrong before, what was measured, what was checked on the server side, and what is **not** verified.
Claiming a test result you did not run is worse than claiming nothing.

If the change is one a user would notice, add a line to [`CHANGELOG.md`](../CHANGELOG.md) under
`Unreleased`, written for them rather than for us: what is different now, and the number if there is
one. A changelog assembled from commit subjects at release time is how release notes come to say
"refactor the frame path".

## What is known to be missing

Kept here rather than in a stale issue tracker:

- **No congestion control.** Video is sent at the configured bitrate whatever the path can carry. The
  protocol reserves the feedback fields; nothing fills them in. Measured consequence: on a 2 Mbit/s
  path a 6 Mbit/s share delivers no complete frames at all.
- **No independent security review** of the video crypt state or of any media parser.
- **No window sharing on the X11 fallback, and no region sharing through the portal.** No single
  Linux configuration offers both.
- **The Windows CI job never runs a test.** It cross-compiles and uploads installers.
- **The performance budget in CI has never been seen to run.**
  [`benchmark.yml`](../.github/workflows/benchmark.yml) builds the client, runs the rig and checks
  `scripts/benchmark/budgets.json`, and every part of it is verified from an empty directory on a
  machine that had never run it - but GitHub's runner is not, because Actions cannot be run from
  there. Its header names the three places it is most likely to need a second attempt.
- **Picture quality is not in the budget.** VMAF, PSNR and SSIM need a second run with frame dumping,
  which roughly doubles the job; latency, frame rate, CPU and memory are checked, quality is not.
