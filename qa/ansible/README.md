# mumble-video QA rig (Ansible)

Stands up a **self-contained** QA environment for mumble-video on a target host:
a Mumble server built from a chosen branch listening on **loopback**, plus N
headless mock clients pointed at it.

## Why loopback

Video in mumble-video is UDP-only (the server never tunnels it over TCP). A host
whose network drops large UDP to the internet - the cursorvm agents box was
measured passing 12-byte pings but dropping 400/900/1200-byte datagrams - can
never display remote video against a public server. Running the server on the
same host and connecting clients to `127.0.0.1` keeps every video datagram on
`lo` (MTU 65536), so real inter-client video works regardless of the host's
external firewall. It also lets one host exercise the server-side fixes
(channel-visibility, stale-stream teardown) with no second machine.

## Use

```bash
cd qa/ansible
ansible-playbook site.yml                         # build+run server, launch clients
ansible-playbook site.yml --tags server           # just the server
ansible-playbook site.yml --tags clients          # just relaunch clients
ansible-playbook site.yml -e mumble_video_ref=main # test a different branch
ansible-playbook site.yml -e qa_client_count=6
ansible-playbook site.yml -e qa_flatpak_bundle=/path/to/mumble-video.flatpak
```

Defaults target `localhost` (connection: local) - run it *on* the box under test.
To drive a remote QA host instead, add it to `[qa]` in `inventory.ini` with
`ansible_host`/`ansible_user`.

## What it creates (under `~/mumble-qa/`)

| Path | What |
|---|---|
| `src/`, `build/` | source checkout and the built `mumble-server` |
| `server-data/` | server ini, sqlite db, log |
| `run/run-server.sh`, `run/stop-server.sh` | loopback server lifecycle (supervised, no systemd) |
| `run/launch-clients.sh`, `run/stop-clients.sh` | client lifecycle |
| `run/logs/` | one stderr log per client, plus Xvfb logs |
| `run/configs/qa-mvN/` | per-client config dir (one instance each) |

The server runs as a supervised background process (`run/run-server.sh`, no
systemd required - works under tini/containers too). Clients launch
with `MUMBLE_MOCK_CAMERA=1` and `MUMBLE_MOCK_SCREEN=1` so they stream without a
camera or a desktop portal. Stopping is scoped to `config/qa-mv*`, so a real
user's own Mumble client on the same host is left alone.

## Requirements

- Ansible on the controller; `sudo` on the target for the package-install tasks.
- The client bundle installed for the user, or pass `qa_flatpak_bundle`.

## Known test-environment limits

- **Channel-visibility (server-side) needs a sub-channel.** A fresh server has only
  Root, so the "move between channels" half of the visibility fix can't be exercised
  until a sub-channel exists. Create one from a connected client (right-click Root ->
  Add), or seed one before first use. The mid-share-join half works on Root alone.
- **True fullscreen needs a compositor.** Under bare Xvfb there is no window manager,
  so a client can't actually go fullscreen; the fullscreen-CPU lane still yields useful
  per-client CPU numbers but not a real fullscreen window. Run the clients under a
  nested compositor (sway/cage) if you need to exercise the fullscreen path itself.
- **Fullscreen** aside, the rig is now fully headless: `seed-clients.sh` generates a
  per-client PKCS12 identity, writes a `mumble_settings.json` with the audio/video/ping
  wizards pre-dismissed, and pre-trusts the loopback server's certificate (SHA1 digest
  in the client's own `cert` sqlite table), so no cert/consent/trust dialog blocks a
  launch. Clients run with `-m --filesystem=<qa_base_dir>`.
