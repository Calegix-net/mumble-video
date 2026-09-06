# Remaining bugs and Windows capture — 2026-09-05

Follow-up: [fixes and patched Windows validation](video-fixes-2026-09-05.md).

Reviewed source: `a5b462b3a` plus the local `fix/video-review` changes.
Windows runtime: Windows 11 Pro, build 26200, Red Hat QXL driver 10.0.0.21000.
The VM passed a native D3D11 hardware-device creation probe with BGRA support
(HRESULT `0x00000000`, feature level `0xB100`).

## Windows run

Tested the downloaded `dev-latest` Windows x86-64 ZIP, whose asset was created
2026-09-01. SHA-256:
`103bd1e7522f2ab1ed5f2e90782dd95bcb3a500671347de273b4f6b6f73b3cb2`.
The executable reports 1.7.0. This is a released binary, **not a Windows build of
the local fixes**. The server was the locally built review server, bound to
loopback on port 64745 and reached through the VM's host gateway.

| Check | Observed result |
| --- | --- |
| Connect Windows client to local server | Passed; server authenticated `capture-test`. |
| Capture the display | Passed; live recursive desktop preview rendered. |
| Stop display capture, start window capture | Passed; command-prompt window rendered with Windows capture border. |
| Maximize the captured window | Failed; preview retained its previous landscape dimensions and clipped the enlarged window, including the right-hand title-bar controls. |
| Include system audio | Failed; client reported `Could not begin activating per-application audio capture`. Video continued. |

Screenshots: [display preview](video-review-evidence/display-preview.png),
[window preview](video-review-evidence/window-preview.png),
[window after maximizing](video-review-evidence/window-resized.png),
[audio activation errors](video-review-evidence/audio-error.png).
These are sender-preview checks. A second receiver, audible playback, physical
camera, portrait monitor, and a Windows build of the patched branch were not
validated by this run.

## Critique, ranked

1. **P1 — Quiet captures cannot satisfy recovery requests and can expire.**
   `DxgiDisplayVideoSource.cpp:262` returns without a callback when the desktop is
   unchanged. `VideoBroadcaster.cpp:81` only resets the encoder on a keyframe
   request, waiting for another callback to encode anything. The server's
   `Server.cpp:2486` silence threshold assumes periodic tile refresh produces
   traffic, but that refresh also requires encode calls. A late viewer can remain
   blank until something changes; a sufficiently idle watched stream can be
   withdrawn by the server. A probe against the actual compiled broadcaster
   produced 6 initial units, **0 recovery units without a capture callback**, and
   6 units when callbacks resumed. Fix recovery using the latest valid frame and
   distinguish healthy idle capture from capture failure in the liveness design.
   Simply increasing the server timeout does not correct the mismatch.

2. **P1 — Window resizing clips content or copies undefined pixels.**
   `WgcWindowVideoSource.cpp:275` allocates the initial-sized pool. The capture
   path at lines 414–474 copies texture dimensions without reading `ContentSize`
   or calling `Recreate`. Maximizing the captured terminal reproduced clipping.
   Shrinking can include undefined pixels outside the valid content rectangle;
   that consequence is established by the API contract, not this screenshot.
   Copy only valid content and recreate the pool when content dimensions change.
   [Microsoft's capture guidance](https://learn.microsoft.com/en-us/windows/apps/develop/media-authoring-processing/screen-capture)
   explains both size handling and pool recreation.

3. **P1 — Screen-share audio fails to activate in the Windows run.**
   Both full-system audio excluding Mumble and selected-application audio use
   `WasapiProcessLoopbackSource`. Its completion handler's `QueryInterface`
   (`WasapiProcessLoopbackSource.cpp:32`) supports only IUnknown and the completion
   interface; it does not aggregate a free-threaded marshaler. This violates
   [Microsoft's required callback agility](https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nn-mmdeviceapi-iactivateaudiointerfacecompletionhandler).
   The runtime activation failure is confirmed; attributing that failure solely
   to marshaling still needs an HRESULT-bearing trace or a Windows before/after
   build. Make the handler agile and include the HRESULT in diagnostic logging.

4. **P2 — A malformed settings schema aborts startup.**
   The test's initial JSON profile omitted `settings_version`. Windows recorded
   a fatal application exit in `libstdc++-6.dll` (`0x40000015`).
   `JSONSerialization.cpp:175` uses `j.at(settings_version)`, while
   `Settings.cpp:280` catches only JSON parse errors. Valid JSON with a missing
   required key therefore escapes recovery. Adding `settings_version: 1` and
   using the correct settings groups allowed the test to proceed. Catch schema
   errors as well as syntax errors and follow the existing backup/default path.
   Earlier launches also recorded a Qt6Core access violation, whose cause remains
   unassigned; it is not evidence of a capture-engine crash.

5. **P2 — Display rotation and separately rendered cursor are omitted.**
   The DXGI source copies the acquired texture without applying output rotation
   or compositing pointer metadata. Portrait displays and hardware-composited
   pointers therefore need explicit handling. These are source findings, not
   reproduced on this landscape VM. Follow the rotation and pointer paths in
   [Microsoft's Desktop Duplication guidance](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/desktop-dup-api).

## A suspicion the runtime test did not confirm

The current WGC source calls `Create` without explicitly creating a
DispatcherQueue. Microsoft's
[CreateFreeThreaded documentation](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.direct3d11captureframepool.createfreethreaded?view=winrt-26100)
identifies it as the option that removes this dependency. However, window capture
**started successfully in the downloaded release**. Treat this as a portability
review item, not a demonstrated startup blocker. Verify the current source build
and its Qt runtime before claiming the dispatcher prevents capture.

The earlier JPEG worker-ordering fix also does not establish UDP sequence
ordering: it preserves decode-submission order. Packet reordering needs separate
validation before claiming all stale-tile artifacts have been fixed.

Native diagnostic logs are saved under `/tmp/mumble-capture-results/`; the idle
probe source is included in `video-review-evidence/idle-capture-probe.cpp`.
No release was published or deployed during this follow-up.

Capture was stopped, the local server and diagnostic HTTP receiver were shut
down, and temporary test media was ejected. The Windows VM was left running.
Its originally configured boot CD referenced a missing file and was ejected to
allow startup; the separate VirtIO driver CD was retained.
