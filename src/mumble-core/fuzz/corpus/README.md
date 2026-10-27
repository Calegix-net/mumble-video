# Fuzz corpus

One directory per fuzz target: `crypt`, `demux`, `reassembly`, `receive`. Every file in them is one
input, and every one of them is replayed by `ctest` on an ordinary build. Adding a file adds a test
case.

Two kinds of file live here.

**Seeds**, which came from `MumbleFuzzMakeSeeds`. They are made by the real fragmenter and the real
crypt state, so they are valid packets rather than random bytes — a fuzzer given valid packets spends
its budget one field away from valid, which is where the interesting arithmetic is, instead of failing
the Protobuf parse a million times. Regenerate them after changing a wire format:

```
./build/tests/MumbleFuzzMakeSeeds src/mumble-core/fuzz/corpus
```

That is deterministic, so it produces no diff unless something really did change.

**Findings**, which a fuzzer produced and a human kept. When one of the targets crashes, libFuzzer
writes the input to `crash-<hash>`; the input belongs here, renamed to say
what it was, and the bug belongs fixed. After that the file is a regression test for free, on every
build, for as long as the repository exists. Keep them small — libFuzzer's `-minimize_crash=1` will
usually cut one down to a few bytes, and a short input is a readable bug report.

## Running the fuzzers for real

```
cmake -S src/mumble-core -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ -DMUMBLE_CORE_FUZZERS=ON
cmake --build build-fuzz
./build-fuzz/MumbleFuzzreassembly src/mumble-core/fuzz/corpus/reassembly -max_total_time=3600
```

Address sanitizer is on in that build, because most of what there is to find in a parser is a read a
few bytes past the end of something, and without ASan that is a pass.
