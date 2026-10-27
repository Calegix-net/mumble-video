# Mumble Video server in Podman

A self-contained image for the [mumble-video](https://github.com/Calegix-net/mumble-video) server.
No compiling: it downloads the project's official prebuilt server (v1.10.0), verifies its checksum,
and writes a working config on first start.

## 1. Build the image (about a minute)

Put `Containerfile` and `entrypoint.sh` in one folder, then from that folder:

```bash
podman build -t mumble-video-server .
```

## 2. Run it

```bash
mkdir -p ~/mumble-data
podman run -d --name mumble-video \
  --network=host \
  -v ~/mumble-data:/data:Z \
  -e SUPERUSER_PASSWORD='choose-a-strong-one' \
  mumble-video-server

podman logs -f mumble-video
```

`podman ps` should show it as "Up". The config and database live in `~/mumble-data` on the host,
so they survive rebuilds and updates. Edit `~/mumble-data/mumble-server.ini` and run
`podman restart mumble-video` to change settings.

Optional first-run settings (ignored once the ini exists): `-e SERVER_PASSWORD=...`,
`-e WELCOME_TEXT=...`, `-e MAX_USERS=...`, `-e SERVER_NAME=...`, `-e PORT=...`.

**Why `--network=host`?** It's what the project's own deploy script uses. Voice and video run over
UDP, and the server needs to see each client's real address; port-forwarding can hide it. If you'd
rather publish ports, swap `--network=host` for `-p 64738:64738/tcp -p 64738:64738/udp`.

## 3. Open the port

Allow **64738 TCP and UDP** through the VM's firewall (and forward it on the router if friends
connect from outside). On Fedora/RHEL:

```bash
sudo firewall-cmd --permanent --add-port=64738/tcp --add-port=64738/udp && sudo firewall-cmd --reload
```

## 4. Recommended: bigger UDP buffers for video

The server asks for 4 MB socket buffers; stock Linux silently caps them far lower, which shows up
as dropped video tiles. Once, on the host:

```bash
echo -e 'net.core.rmem_max=8388608\nnet.core.wmem_max=8388608' | sudo tee /etc/sysctl.d/90-mumble-video.conf
sudo sysctl --system
podman restart mumble-video
```

## Start on boot (optional)

Use the included `mumble-video.container` Quadlet file. Instructions are at the top of that file.
(Remove the manually started container first: `podman rm -f mumble-video`.)

## Updating

Change `VERSION` and `SHA256` at the top of `Containerfile` (the hash is in the release's
`SHA256SUMS` file), rebuild, then `podman rm -f mumble-video` and run step 2 again. Your data is kept.

## Notes

- Everyone must use the **mumble-video client** from the project's releases page; regular Mumble
  clients can't connect to this server.
- Log in as `SuperUser` with the SUPERUSER_PASSWORD to manage channels and permissions.
- A log line warning about running as root is expected and harmless under rootless Podman: "root"
  in the container is just your own user on the host.
