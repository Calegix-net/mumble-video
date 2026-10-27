// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

/*
 * Fuzzes MediaDemux::classify(), which is the very first thing a datagram meets.
 *
 * It is a handful of lines and looks beneath fuzzing. It is included anyway for one reason: it hands
 * out two spans into the caller's buffer, and the whole audio and video stack is downstream of those
 * being right. A payload span one byte too long here is a read past the end of a UDP buffer somewhere
 * else entirely, and the only cheap way to be sure is to state the invariant and let a fuzzer argue.
 *
 * Both framings are exercised, because a connection to stock Mumble uses the other one.
 */

#include "FuzzInput.h"

#include "MediaDemux.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>

using namespace Mumble::Protocol;

namespace {

void check(bool condition) {
	if (!condition) {
		std::abort();
	}
}

/// Whether a span lies entirely inside another, which is what "a view of the caller's buffer" means.
bool within(std::span< const std::uint8_t > inner, std::span< const std::uint8_t > outer) {
	if (inner.empty()) {
		return true;
	}

	return inner.data() >= outer.data() && inner.data() + inner.size() <= outer.data() + outer.size();
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
	const std::span< const std::uint8_t > whole(data, size);

	for (const MediaDemux::Framing framing : { MediaDemux::Framing::ChannelByte, MediaDemux::Framing::Plain }) {
		const MediaDemux demux(framing);
		const MediaDatagram classified = demux.classify(whole);

		// Neither span may reach outside what it was given, and neither may be longer than it.
		check(within(classified.datagram, whole));
		check(within(classified.payload, whole));
		check(classified.datagram.size() <= size);
		check(classified.payload.size() <= size);

		// An unknown channel is the answer for anything that is not addressed to audio or video, and
		// nothing downstream looks at the spans in that case. Everything else must have classified a
		// datagram, and must not claim a payload longer than it.
		if (classified.channel != MediaChannel::Unknown) {
			check(!classified.datagram.empty());
			check(classified.payload.size() <= classified.datagram.size());
		}
	}

	return 0;
}
