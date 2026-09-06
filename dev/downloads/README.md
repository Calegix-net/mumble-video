# Direct Mumble downloads

`index.html` is served at <https://calegix.com/dl/mumble/> by the existing
`mumble-dl` nginx container on calegix.com.

Publish ZIPs and `SHA256SUMS` into a dated directory below
`/home/natej/mumble-fork/webroot/`. Upload complete files to a staging directory,
verify their hashes, then move that directory into place before updating
`index.html`. Keep historical dated directories intact. Verify HTTP downloads and
range requests after publishing; do not change unrelated Traefik routes.

The source branch is `Calegix-net/mumble-video:fix/video-review`. Linux builds use
`docker/Dockerfile` (Fedora 42, glibc 2.41); Windows builds use
`docker/Dockerfile.windows`. Run the CTest suite and package the matching runtime
libraries with the repository's bundle scripts. Write the source commit and binary
SHA-256 into each bundle's `BUILD-INFO.txt`.

Publishing downloads does not deploy the production Mumble server.
