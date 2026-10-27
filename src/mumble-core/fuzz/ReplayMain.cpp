// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

/*
 * A main() for the fuzz targets, used when libFuzzer is not available.
 *
 * Fuzzing proper needs clang, and the point of a fuzzer is to run for hours, so neither belongs in a
 * pull request's CI. What does belong there is the corpus: once an input has crashed something, it
 * must never crash it again, and checking that takes milliseconds and no special compiler.
 *
 * So every build gets these targets as plain programs that replay the files named on the command line
 * through LLVMFuzzerTestOneInput and exit zero if none of them brings the process down. That is a
 * regression test, it runs under ctest with everything else, and it keeps the harness itself compiling
 * on every platform rather than rotting until the next time somebody reaches for clang.
 *
 * Builds with MUMBLE_CORE_FUZZERS=ON link libFuzzer's own main instead and this file is left out.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size);

int main(int argc, char **argv) {
	// No arguments is a pass, not a usage error: it is what an empty corpus directory expands to, and a
	// corpus that has not needed an entry yet should not fail the build.
	for (int i = 1; i < argc; ++i) {
		std::FILE *file = std::fopen(argv[i], "rb");

		if (!file) {
			std::fprintf(stderr, "replay: cannot open %s\n", argv[i]);
			return 1;
		}

		std::vector< std::uint8_t > input;
		std::uint8_t buffer[4096];
		std::size_t got = 0;

		while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
			input.insert(input.end(), buffer, buffer + got);
		}

		std::fclose(file);
		LLVMFuzzerTestOneInput(input.data(), input.size());
	}

	return 0;
}
