# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

add_executable(video_benchmark "video_benchmark.cpp")

target_link_libraries(video_benchmark PRIVATE shared)

target_link_libraries(video_benchmark PRIVATE benchmark::benchmark)
