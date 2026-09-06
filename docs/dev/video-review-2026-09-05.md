# Client and server video review — 2026-09-05

Follow-up: [fixes and patched Windows validation](video-fixes-2026-09-05.md).

Reviewed `Calegix-net/mumble-video` main at `a5b462b3a` in the isolated
`fix/video-review` worktree. The existing `mumble-plus` checkout was preserved.

## Findings addressed

| Priority | Trigger and failure | Change |
| --- | --- | --- |
| P1 | A remote stream is announced while the video dock is hidden. No notification exposes the dock, and no frames arrive until the inaccessible Watch button is clicked. | Announcing a new surface emits the total tile count and lays out its controls. |
| P1 | TCP stream updates, disconnects, or timeout cleanup mutate router maps while the UDP relay reads or updates them. The control path did not acquire the lock held by the relay. | Protect router mutations with the voice-thread write lock and use the synchronized ACL lookup helpers. This race was identified by code inspection; a ThreadSanitizer run was not performed. |
| P2 | Stop watching receives a normal `subscribe=false` acknowledgement. The client treats it as a revoked stream and deletes the tile. A delayed acknowledgement can also cancel a subsequent Watch click. | Track outstanding local unwatch requests and consume their acknowledgements while preserving the advertised tile and newer watch intent. Unsolicited withdrawals retain their existing handling. |
| P2 | The last remote sender disappears while the local camera or screen preview is still active. A remote-only count of zero hides the whole dock. | Removal notifications include local previews. |
| P2 | A video subscription is denied. The server sends only `PermissionDenied`, although the viewer waits for a stream-specific subscription reply. | Also send `VideoSubscribe(subscribe=false)` with the matching session and stream, and clear the keyframe-request flag. |
| P2 | A viewer repeatedly requests recovery for a silent stream. Every re-subscribe resets the server's 30-second silence baseline, preventing cleanup. | Grant a new grace period only when an unwatched stream gains a viewer. |
| P2 | Video subscriptions are revalidated before the ACL cache is cleared. Old cached permissions can prevent the intended withdrawal notification. | Revalidate after cache invalidation, outside the cache mutex, preserving lock order. |
| P2 | Background JPEG decodes finish out of order. An older tile can overwrite a newer tile; results from before an unwatch can appear after watching resumes. | Paint completed tiles in submission order, discard queued results on unwatch or stream replacement, and cap queued/in-flight decode tasks. |

## Validation

The regression tests reproduced the hidden-dock, local-preview disappearance, and
missing subscription-denial reply before their fixes. Additional tests cover
delayed unwatch replies, out-of-order worker completion, discarded pending tiles,
and silent-stream expiration despite repeated recovery requests.

All 30 CTest suites passed across the full run and a targeted rerun. The full run
passed 29 suites; the integration suite initially needed a larger timing allowance
because the server checks timeouts every 15.5 seconds. After allowing for that
polling interval, `TestVideoCall` passed all eight QtTest entries in 41.93 seconds.
Changed C++ files also passed clang-format verification and Git whitespace checks.

Built the complete Linux client, server, and tests with Qt 6.11.1. Configuration:

```sh
cmake -S . -B build-native-review \
  -Dtests=ON -Dice=OFF -Doverlay=OFF -Dplugins=OFF -Dlto=OFF \
  -Dwarnings-as-errors=OFF -Dbundled-rnnoise=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build build-native-review -j 8
QT_QPA_PLATFORM=offscreen ctest --test-dir build-native-review --output-on-failure -j 6
```

The integration tests launch a temporary server bound to loopback and use real
TLS clients, session-derived media encryption, and UDP video delivery. The test
configuration disables automatic connection bans because the expanded suite
intentionally creates more than ten clients from the same loopback address.
This changes only the temporary test server's configuration.

## Scope and remaining validation

This review tested a locally built server, not the deployed service. No release
was published or deployed. A subsequent [Windows capture run](video-review-windows-2026-09-05.md)
verified display and window previews and found audio activation and resize failures.
Physical cameras, desktop portals, and a live multi-person call still need device testing. Existing packet-loss and
codec tests do not establish behavior under every real network condition.

The upstream tree contains twelve files whose CRLF blobs conflict with its LF
attributes. Git reports these as modified even though their bytes match `HEAD`;
they are unrelated to this review. Use `git diff --ignore-space-at-eol` to inspect
the substantive changes.
