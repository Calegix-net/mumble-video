# Windows capture regression

This interactive device test compiles the production Windows capture sources.
Run it inside an unlocked Windows desktop with a working display and audio device.
It opens and resizes a colored test window, captures the display, plays a short
440 Hz tone in a child process, and checks application audio for non-silent PCM.
It also checks activation of system capture excluding its own process tree.

Build with the repository's Windows cross-build image:

```sh
mingw64-cmake -S /src/dev/windows-capture-smoke -B /smoke \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=/usr/bin/x86_64-w64-mingw32-g++
cmake --build /smoke -j2
```

Run `WindowsCaptureSmoke.exe` alongside the matching Qt runtime DLLs and the
`platforms/qwindows.dll` plugin. The executable writes PASS/FAIL lines and captures
`capture-initial.png`, `capture-grown.png`, `capture-shrunk.png`, and
`capture-display.png` in its working directory. Exit status is nonzero if a check
fails. `--tone` is an internal mode used to create the child audio source.

These are capture-backend checks. The Linux CTest suite separately tests encoding,
stream replacement, receiver behavior, and encrypted server relay. A physical
portrait display and listening to a full remote call remain useful device checks.
