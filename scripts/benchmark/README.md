# The video benchmark

A server and two clients on virtual displays, one sharing a deterministic mock desktop, with the
loopback between them shaped by `tc netem`. It reports frame rate, bitrate, encode time, latency,
CPU, memory, GPU, retransmission counters and picture quality, under no impairment and under four
kinds of bad network.

It exists because **a change to this pipeline that is not measured is not finished**. Every
performance regression this code has had was found by somebody deciding to measure, which means the
ones nobody measured are still in here.

## Run it

```sh
sudo apt install xvfb ffmpeg imagemagick sqlite3 openssl iproute2 util-linux python3-venv ninja-build
cmake -S . -B build && cmake --build build          # the client and server it measures

scripts/benchmark/setup-rig.sh                       # once
scripts/benchmark/build-vmaf.sh                      # once, and only for picture quality
scripts/benchmark/bench.sh mine 40 auto clean loss2 delay100 bw2mbit bad
```

Results land in `~/.cache/mumble-video-bench/out/mine/` as `summary.txt` (a table) and `results.json`.
To compare two of them:

```sh
scripts/benchmark/compare.py before after
```

A metric is only called better or worse when it moved by more than both a relative and an absolute
threshold, because this rig's CPU and timing figures wander by a couple of per cent between identical
runs and reporting that as a result would be worse than reporting nothing.

### Everything is overridable

`env.sh` is the only place paths are decided, and every value takes an environment variable:
`MUMBLE_SRC`, `MUMBLE_BUILD`, `MUMBLE_BENCH_WORK`, `MUMBLE_BENCH_PYTHON`, `MUMBLE_VMAF`. Nothing is
written outside `MUMBLE_BENCH_WORK`, so a benchmark never shows up in `git status`.

`MUMBLE_BENCH_HOST` and `MUMBLE_BENCH_PORT` exist but changing them means running `setup-rig.sh`
again — see below.

## The five scenarios

| Scenario | `tc netem` | What it is for |
|---|---|---|
| `clean` | none | The ceiling. Any regression shows here first and with the least noise. |
| `loss2` | `loss 2%` | Retransmission, keyframe recovery, the hold-and-drain path. |
| `delay100` | `delay 100ms` | 200 ms round trip: whether anything waits for a round trip it should not. |
| `bw2mbit` | `rate 2mbit` | A 3 Mbit/s share on a 2 Mbit/s path. There is no congestion control, so this is the honest demonstration of that. |
| `bad` | all three | Everything at once. |
| `drop` | 100% loss for 5s, mid-window | Recovery. The other four are *steady* bad links; this one takes the link away and gives it back. |

## Why it is built this way

**A network namespace, not the real loopback.** `unshare -r --net` maps the caller to uid 0 inside a
new user namespace, which grants `CAP_NET_ADMIN` over that namespace's own interfaces and nothing
else on the machine. So `tc` can shape loopback without root and without touching the host's
networking. The server, both clients and both X servers all start inside it, so all of their traffic
crosses the shaped link.

**Performance and quality are separate runs of the same scenario.** Saving a PNG per frame costs real
CPU and real disk, so a run that measures CPU must not be dumping frames, and a run that dumps frames
cannot be trusted for CPU. Both are short enough that doing each once is cheaper than arguing about
how much the dumping cost.

**Frames are paired by the sender's frame number, not by position.** Both ends name their dumps with
the number that travels on every unit, so `tx-<stream>-<n>.png` and `rx-<stream>-<n>.png` are the same
frame before and after the pipeline. Only numbers present on both sides are scored: a frame the
receiver never got is a loss, the receive counters already report it, and feeding it to a quality
metric as a black frame would report that loss twice and call it blur.

**There is a settling period and it is excluded.** Connecting, the first keyframe burst and the
encoder finding its rate are start-up. Averaging them in would describe a moment nobody watches.

**The mock desktop is deterministic.** `MUMBLE_MOCK_SCREEN` draws a static wallpaper, a terminal whose
text scrolls one line every four frames, a block that moves every frame and a frame counter — mostly
unchanged pixels with a few regions always changing, which is what a shared desktop actually looks
like to a screen codec. The scrolling text is the content such a codec is judged on. Nothing here
depends on what is on the developer's actual screen.

**The clients are asked repeatedly to start, not once.** A client that has not finished connecting
accepts the request and does nothing useful with it, and no log line reliably says "connected". The
statistics lines only appear once frames are moving, so those are what the harness waits for.

## The one genuinely awkward part

A Mumble client asks before trusting a server's certificate, and a headless client has nobody to ask,
so it waits at "Not connected" for ever. `setup-rig.sh` starts the server once, reads the certificate
fingerprint off the socket with `openssl`, and writes the trust row into each client's database before
the client has ever run.

That trust is stored against a hostname and port, which is why `MV_HOST` and `MV_PORT` are fixed: the
pair is the key. Change them and the rig has to be built again.

## Longer questions

```sh
scripts/benchmark/profile.sh 25                 # perf: flat profiles and folded stacks, for flame graphs
scripts/benchmark/soak.sh 120                   # two hours, with a memory slope fitted per process
```

A forty-second run cannot answer either of those. A leak of a few kilobytes a second is invisible in
forty seconds and obvious in an hour, and a flat profile needs enough samples to be a profile.

## Budgets

```sh
scripts/benchmark/bench.sh ci 20 auto clean drop
scripts/benchmark/budget.py ci
```

`budget.py` checks a run against `budgets.json` and exits non-zero if anything is over. Each budget
records what the metric actually measures, so the headroom is visible rather than implied, and a
missing metric fails rather than passes — a budget that silently stops being checked is worse than one
that fails.

It compares against absolute limits rather than against a previous run, on purpose. CI runners are
shared, and a comparison between two different runners is a comparison of two runners. Absolute
limits with real headroom say something true on any machine. For the small differences, use
`compare.py` on a developer rig, where before and after are taken on the same machine minutes apart.

Only metrics that survive a shared runner are budgeted: whether frames arrive, whether they arrive on
time, whether memory is where it should be, and whether the picture comes back after the link does.
Absolute CPU is not one of them.

`.github/workflows/benchmark.yml` runs exactly those two commands. **That workflow has never
executed** — the budget checker and every script it drives are tested, on a developer machine and in
WSL, but the YAML and the runner are not. Its header says what is most likely to need a second
attempt.

## Files

| File | |
|---|---|
| `env.sh` | The only place paths are decided. Sourced by everything. |
| `setup-rig.sh` | Builds the rig from nothing. Run once. |
| `bench.sh` | Runs a set of scenarios and summarises them. The entry point. |
| `inner-bench.sh` | One run, from inside the network namespace. Not called directly. |
| `quality.sh` | VMAF, PSNR and SSIM over frame-matched pairs. |
| `summarise.py` | Raw logs and `/proc` samples into `summary.txt` and `results.json`. |
| `compare.py` | Two runs side by side, as text or as HTML rows. |
| `benchpaths.py` | Where runs live, for the Python that is not a child of a shell that sourced `env.sh`. |
| `budget.py`, `budgets.json` | The CI guardrail. |
| `profile.sh`, `soak.sh` | Where the time goes; whether memory grows. |
| `build-vmaf.sh` | libvmaf's CLI, because Ubuntu's ffmpeg has no libvmaf filter. |
| `resummarise.sh` | Re-derives summaries for runs already on disk, after a summariser fix. |
