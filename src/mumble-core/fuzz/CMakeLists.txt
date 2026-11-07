# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

# Fuzz targets for the three things that read bytes off the network, in two shapes from one source.
#
# By default each target is built as a corpus replayer: ReplayMain.cpp supplies a main() that runs the
# files named on its command line, and ctest runs each target over its committed corpus. That costs
# milliseconds, needs no particular compiler, and is what stops a crash anybody has already found from
# coming back.
#
# With MUMBLE_CORE_FUZZERS=ON and a compiler that has -fsanitize=fuzzer, the same sources link
# libFuzzer's main instead and become real fuzzers. That is a developer's or a CI cron job's build, not
# a pull request's, because fuzzing is only useful when it runs for hours.

if(MUMBLE_CORE_FUZZERS)
	include(CheckCXXSourceCompiles)

	set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer")
	check_cxx_source_compiles(
		"#include <cstddef>
		 #include <cstdint>
		 extern \"C\" int LLVMFuzzerTestOneInput(const uint8_t *, size_t) { return 0; }"
		MUMBLE_CORE_HAVE_LIBFUZZER
	)
	unset(CMAKE_REQUIRED_FLAGS)

	if(NOT MUMBLE_CORE_HAVE_LIBFUZZER)
		message(FATAL_ERROR
			"MUMBLE_CORE_FUZZERS=ON needs a compiler with -fsanitize=fuzzer (clang). Leave it off to build "
			"the same targets as corpus replayers instead."
		)
	endif()
endif()

# name is both the target suffix and the corpus subdirectory, so corpus/<name>/ is where a fuzzer's
# findings go.
function(mumble_core_add_fuzz_target name source)
	set(target "MumbleFuzz${name}")

	add_executable(${target} "${source}" "FuzzInput.h")
	target_link_libraries(${target} PRIVATE mumble-core)

	if(MUMBLE_CORE_FUZZERS)
		# Address sanitizer alongside the fuzzer: most of what a fuzzer finds in code like this is a read
		# or a write just past the end of something, which without ASan is a pass.
		#
		# UndefinedBehaviorSanitizer as well, because ASan does not look for the other half of what this
		# code could plausibly get wrong: overflow and sign conversion in a length or an offset, a shift
		# past the width of its type, a misaligned load of a packet field, or pointer arithmetic that
		# leaves its object. Finding one of those was not hypothetical - enabling it here is what turned
		# up out-of-bounds pointer arithmetic in the AES wrapper that had been running on every packet.
		#
		# -fno-sanitize-recover makes undefined behaviour abort rather than print and continue, which is
		# what gets libFuzzer to stop and write a reproducer instead of burying the report in the log.
		# vptr is left out: it needs every translation unit instrumented, which the system libraries are
		# not, and so reports nothing but false positives here.
		target_compile_options(${target} PRIVATE
			-fsanitize=fuzzer,address,undefined
			-fno-sanitize=vptr
			-fno-sanitize-recover=undefined
			-fno-omit-frame-pointer
		)
		target_link_options(${target} PRIVATE -fsanitize=fuzzer,address,undefined)
	else()
		target_sources(${target} PRIVATE "ReplayMain.cpp")

		# The corpus is expanded at configure time rather than passed as a directory, so that adding a
		# file is noticed by CMake and so that the failing test's command line names the input.
		file(GLOB corpus CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/corpus/${name}/*")
		add_test(NAME ${target} COMMAND ${target} ${corpus})
	endif()

	if(MUMBLE_CORE_IN_MUMBLE_TREE)
		set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
	endif()
endfunction()

mumble_core_add_fuzz_target(reassembly "FuzzVideoReassembly.cpp")
mumble_core_add_fuzz_target(crypt "FuzzVideoCrypt.cpp")
mumble_core_add_fuzz_target(demux "FuzzMediaDemux.cpp")
mumble_core_add_fuzz_target(receive "FuzzVideoReceive.cpp")

# The seed generator, which is an ordinary program in every build: the seeds are made by the real
# fragmenter and crypt state, so they stay correct when a wire format changes, and regenerating them is
# one command rather than a note in a README.
add_executable(MumbleFuzzMakeSeeds "MakeSeeds.cpp")
target_link_libraries(MumbleFuzzMakeSeeds PRIVATE mumble-core)

if(MUMBLE_CORE_IN_MUMBLE_TREE)
	set_target_properties(MumbleFuzzMakeSeeds PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
endif()
