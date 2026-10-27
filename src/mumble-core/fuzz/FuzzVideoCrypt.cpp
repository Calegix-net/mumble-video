// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

/*
 * Fuzzes VideoCryptState::decrypt(), the first code a video datagram off the socket reaches.
 *
 * Almost every input here is rejected, which is the point: the question is not whether a forgery is
 * accepted - OCB2 answers that - but whether rejecting one is safe. decrypt() reads a sequence number
 * and a truncated tag out of the datagram before it can know any of it is genuine, indexes a replay
 * window with that sequence number, and has to cope with datagrams shorter than its own header.
 *
 * Two properties are asserted rather than merely exercised, because both have been wrong in
 * implementations of this shape before:
 *
 *   - A datagram that does not verify must not advance the replay window. Otherwise anyone able to
 *     inject a packet with a high sequence number can lock the real sender out, which is a denial of
 *     service that needs no key.
 *   - A datagram this state itself produced must come back out byte for byte, once.
 *
 * The key is fixed and public. There is no secret here to protect: the target exists to find a crash
 * or a state bug, and a fuzzer that had to guess a key would never reach either.
 */

#include "FuzzInput.h"

#include "VideoTransport.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

using namespace Mumble::Protocol;

namespace {

/// Fixed and arbitrary: a 16-byte key and two distinct 12-byte nonce bases, which is what setKey()
/// wants. Keyed in place rather than returned, because a crypt state holds an OpenSSL context and is
/// deliberately neither copyable nor movable.
void key(VideoCryptState &state, char outgoing, char incoming) {
	if (!state.setKey(std::string(16, '\x2a'), std::string(12, outgoing), std::string(12, incoming))) {
		std::abort();
	}
}

void check(bool condition) {
	if (!condition) {
		std::abort();
	}
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
	MumbleFuzz::FuzzInput input(data, size);

	// Two states with the nonce bases swapped, so one can encrypt what the other decrypts - which is
	// how the real client and server are set up, and the only way to produce a datagram that should
	// verify.
	VideoCryptState receiver;
	VideoCryptState sender;
	key(receiver, '\x01', '\x02');
	key(sender, '\x02', '\x01');

	std::vector< std::uint8_t > plaintext;
	std::vector< std::uint8_t > datagram;

	while (input.remaining() > 0) {
		const std::span< const std::uint8_t > chunk = input.chunk();

		if (input.choice(4) == 0) {
			// A genuine datagram, to prove the state still works after whatever has been thrown at it.
			// Encryption refuses an over-length plaintext, which is not what this is testing.
			if (chunk.size() > MAX_VIDEO_PLAINTEXT_SIZE) {
				continue;
			}

			datagram.clear();

			if (!sender.encrypt(chunk, datagram)) {
				continue;
			}

			plaintext.clear();
			check(receiver.decrypt(datagram, plaintext) == VideoCryptState::Result::Ok);
			check(plaintext.size() == chunk.size());
			check(plaintext.empty() || std::equal(plaintext.begin(), plaintext.end(), chunk.begin()));

			// And once only. Replaying it must be refused, and refused as a replay rather than as a
			// forgery, since the distinction is what the counters report.
			plaintext.clear();
			check(receiver.decrypt(datagram, plaintext) == VideoCryptState::Result::Replay);
			continue;
		}

		// Otherwise: whatever the fuzzer made. The window must not move for anything that fails.
		const std::uint32_t before = receiver.highestReceived();
		plaintext.clear();

		if (receiver.decrypt(chunk, plaintext) != VideoCryptState::Result::Ok) {
			check(receiver.highestReceived() == before);
		}
	}

	return 0;
}
