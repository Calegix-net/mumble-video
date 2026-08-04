# Containerised Linux build

A reproducible Linux build environment for Mumble, so that you can build and run the test suite
without installing any dependencies on your host.

Works with either Docker or Podman. The examples use `docker`; substitute `podman` if you prefer.

## Building the image

From the root of the source tree:

```bash
docker build -t mumble-build -f docker/Dockerfile docker/
```

The image is based on `fedora:42`. To use a different base, pass `--build-arg BASE_IMAGE=...`, though
note that the package names in the `Dockerfile` are Fedora-specific and would need adjusting for a
Debian-derived base.

## Building Mumble

First make sure the submodules are present, otherwise configuration will fail:

```bash
git submodule update --init --recursive
```

Then configure and build. The source tree is mounted read-only at `/src` and all build artefacts go
into a `build/` directory on the host, so the build cannot write into your checkout:

```bash
mkdir -p build
docker run --rm \
    -v "$PWD":/src:ro,z \
    -v "$PWD/build":/build:z \
    mumble-build \
    bash -c "cmake -S /src -B /build -Dtests=ON -Dice=OFF -Doverlay-xcompile=OFF && cmake --build /build -j$(nproc)"
```

The `,z` mount option relabels the volume for SELinux and is needed on Fedora, RHEL and derivatives.
It is harmless elsewhere, but you can drop it if your platform does not use SELinux.

`-Dice=OFF` skips the ZeroC Ice RPC interface and `-Doverlay-xcompile=OFF` skips cross-compiling the
32-bit overlay, neither of which is needed for a normal build or for the tests. See
[cmake_options.md](../docs/dev/build-instructions/cmake_options.md) for the full set of options.

## Running the tests

```bash
docker run --rm \
    -v "$PWD":/src:ro,z \
    -v "$PWD/build":/build:z \
    -e QT_QPA_PLATFORM=offscreen \
    mumble-build \
    ctest --test-dir /build --output-on-failure
```

`QT_QPA_PLATFORM=offscreen` is required because several tests construct Qt widgets and there is no
display inside the container.
