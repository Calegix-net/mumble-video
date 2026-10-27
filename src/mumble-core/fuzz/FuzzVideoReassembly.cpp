// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

/*
 * Fuzzes the video reassembler, which is the furthest a hostile datagram's own bytes get.
 *
 * Everything in front of it either checks a cryptographic tag (VideoCryptState) or looks at a single
 * byte (MediaDemux). This is the first component that reads attacker-chosen numbers and does
 * arithmetic with them: fragment indices, fragment counts, payload lengths, tile coordinates. It
 * allocates in proportion to them, indexes a vector with them, and holds the result across packets.
 *
 * A note on what is in scope. senderSession is *not* taken from the packet - processPacket() takes it
 * from the caller's trusted context, and rejects a packet whose own field disagrees. So this drives it
 * with a small set of session numbers rather than letting the input choose freely: three is enough to
 * exercise the per-sender limits and the cross-sender isolation, and letting the fuzzer pick 2^32 of
 * them would only spend its budget on the map.
 */

#include "FuzzInput.h"

#include "VideoFragmentation.h"

#include <cstddef>
#include <cstdint>
#include <vector>

using namespace Mumble::Protocol;

namespace {

/// Enough sessions to collide and to be isolated from each other, few enough that the fuzzer reaches
/// both cases.
constexpr std::uint32_t SESSIONS[] = { 1, 2, 7 };

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
	MumbleFuzz::FuzzInput input(data, size);
	VideoReassembler reassembler;

	// A clock the input advances but cannot rewind. Reassembly timeouts, NACK delays and NACK retry
	// intervals are all expressed against it, so a monotonic clock under the fuzzer's control is what
	// reaches the expiry and retransmission paths - and a clock that could go backwards would be
	// testing a guarantee the callers do provide.
	std::uint64_t nowUsec = 1;
	VideoUnit unit;
	std::vector< VideoNackRequest > nacks;

	while (input.remaining() > 0) {
		const std::uint32_t session = SESSIONS[input.choice(sizeof(SESSIONS) / sizeof(SESSIONS[0]))];

		// Steps of up to 64 ms, which straddles every interval that matters: the 25 ms NACK delay, the
		// 150 ms retry and the reassembly timeout all become reachable within a handful of packets.
		nowUsec += static_cast< std::uint64_t >(input.byte()) * 256;

		const std::span< const std::uint8_t > packet = input.chunk();
		const VideoReassemblyResult result           = reassembler.processPacket(packet, session, nowUsec, unit);

		if (result == VideoReassemblyResult::Complete) {
			// Touch what a caller touches. A unit that claims a payload it does not own would show up
			// here and nowhere else, since the reassembler itself never reads the bytes back.
			std::size_t sum = 0;

			for (const std::uint8_t value : unit.payload) {
				sum += value;
			}

			// Volatile so the read is not optimised away. The value is not asserted on - there is no
			// right answer for arbitrary input - but the read itself is the point: it is what makes a
			// unit that points outside its own buffer a crash here rather than a surprise in a decoder.
			static volatile std::size_t sink;
			sink = sum;
			// Read back, so the store cannot be dead and the sum really is computed.
			(void) sink;
		}

		// The two maintenance calls a real receiver makes on a timer, driven here by the same input, so
		// that expiry and retransmission run interleaved with arrival rather than only at the end.
		switch (input.choice(4)) {
			case 1:
				reassembler.expire(nowUsec);
				break;
			case 2:
				nacks.clear();
				reassembler.collectNacks(nowUsec, nacks);

				for (const VideoNackRequest &request : nacks) {
					reassembler.noteNackSent(request, nowUsec);
				}

				break;
			case 3:
				reassembler.removeSender(session);
				break;
			default:
				break;
		}
	}

	return 0;
}
