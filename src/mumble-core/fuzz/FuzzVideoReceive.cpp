// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

/*
 * Fuzzes VideoReceiver::receive(), which is the whole client-side receive path in one call: decrypt,
 * read the server's sender stamp, reassemble, and decide what to ask for again.
 *
 * Worth a target of its own even though the crypt state and the reassembler have theirs, because what
 * is interesting here is not any one of those layers but the join between them - what the peek parse
 * believes about a datagram the crypt state has only just accepted, and what the retransmission pacing
 * does when the reassembler reports loss faster than the rate limit allows requests.
 *
 * Datagrams are fed through the real crypt state rather than as plaintext, because receive() will not
 * look at anything that has not authenticated. So a seed is a *plaintext* the target then encrypts,
 * which is also what keeps the seeds valid if the framing changes; a fuzzer's mutations of that
 * plaintext reach the parser, and its mutations of the ciphertext path are covered by the crypt target.
 *
 * Two invariants are asserted, both of them properties a hostile sender should not be able to break:
 * every packet the receiver decides to send must be a legal size for the link, and the number of
 * request messages produced in a second must stay inside the receiver's own rate limit. A receiver
 * that can be made to flood on demand is an amplifier.
 */

#include "FuzzInput.h"

#include "VideoReceiver.h"
#include "VideoTransport.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

using namespace Mumble::Protocol;

namespace {

void check(bool condition) {
	if (!condition) {
		std::abort();
	}
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
	MumbleFuzz::FuzzInput input(data, size);

	// Opposite nonce bases, as a real pair of ends has, so that what one encrypts the other accepts.
	VideoCryptState sending;
	VideoCryptState receiving;
	check(sending.setKey(std::string(16, '\x2a'), std::string(12, '\x01'), std::string(12, '\x02')));
	check(receiving.setKey(std::string(16, '\x2a'), std::string(12, '\x02'), std::string(12, '\x01')));

	VideoReceiver receiver;
	std::vector< std::uint8_t > datagram;

	// Microseconds. Advanced by the input but never backwards, because the rate limiting and the
	// request interval are both expressed against it and the callers do guarantee monotonicity.
	std::uint64_t nowUsec = 1;

	// For the rate limit check: request messages produced since the start of the current second.
	std::uint64_t secondStartUsec = nowUsec;
	unsigned int messagesThisSecond = 0;

	while (input.remaining() > 0) {
		const std::span< const std::uint8_t > plaintext = input.chunk();

		// Steps of up to a quarter second, which straddles the 10 ms request interval and the one-second
		// rate-limit window, so both are reachable within a few packets.
		nowUsec += static_cast< std::uint64_t >(input.byte()) * 1000;

		if (nowUsec - secondStartUsec >= 1000000) {
			secondStartUsec    = nowUsec;
			messagesThisSecond = 0;
		}

		if (plaintext.size() > MAX_VIDEO_PLAINTEXT_SIZE) {
			continue;
		}

		datagram.clear();

		if (!sending.encrypt(plaintext, datagram)) {
			continue;
		}

		// Half the time, corrupt the datagram after it was made. Most such packets are refused by the
		// tag, which is the point: the refusal path runs as often as the accepting one.
		if (input.choice(2) == 0 && !datagram.empty()) {
			datagram[input.byte() % datagram.size()] ^= input.byte();
		}

		VideoReceiveEvents events;
		receiver.receive(receiving, datagram, nowUsec, events);

		for (const std::vector< std::uint8_t > &packet : events.packetsToSend) {
			// It has to fit, or the thing that sends it writes past the end of a datagram buffer. And it
			// has to say something, because an empty packet costs a datagram and asks for nothing.
			check(!packet.empty());
			check(packet.size() <= MAX_VIDEO_PLAINTEXT_SIZE);
		}

		messagesThisSecond += static_cast< unsigned int >(events.packetsToSend.size());

		// Twice the limit, not the limit. The receiver keeps its own one-second window and this one
		// cannot be aligned with it from out here, so a window of mine can straddle the end of one of
		// its windows and the start of the next and legitimately contain a full allowance from each.
		// Asserting the limit itself would make this target report a flood that is not one, which is
		// worse than not asserting it: a harness nobody trusts is a harness nobody runs. Twice the
		// allowance is still a bound, and still catches a rate limit that is not working at all.
		check(messagesThisSecond <= 2 * VideoReceiver::MAX_NACK_MESSAGES_PER_SECOND);

		for (const VideoUnit &unit : events.units) {
			// Read, so that a unit describing a payload it does not own is a crash here rather than a
			// surprise in a decoder.
			static volatile std::size_t sink;
			std::size_t sum = 0;

			for (const std::uint8_t value : unit.payload) {
				sum += value;
			}

			sink = sum;
			// Read back, so the store cannot be dead and the sum really is computed.
			(void) sink;
		}

		if (input.choice(8) == 0) {
			receiver.removeSender(input.byte());
		}
	}

	return 0;
}
