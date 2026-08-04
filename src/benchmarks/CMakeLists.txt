# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

include(FetchContent)

FetchContent_Declare(
  googlebenchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG        v1.6.0
)

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "")

FetchContent_MakeAvailable(googlebenchmark)

# Google Benchmark is a third-party dependency and does not compile cleanly under the strict warning
# set this project builds with: GCC 15 rejects it outright over -Werror=conversion and
# -Werror=sign-conversion in counter.cc and statistics.cc. Suppress warnings for it exactly as is done
# for the other vendored dependencies, otherwise no benchmark can be built at all on a modern compiler.
disable_warnings_for_all_targets_in("${googlebenchmark_SOURCE_DIR}")

add_subdirectory(protocol)
add_subdirectory(AudioReceiverBuffer)
add_subdirectory(video)
