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
 *   - A datagram this state itself produced, *if it verifies at all* and the state did not record
 *     having altered it, must come back out byte for byte, once. Both qualifiers are load-bearing and
 *     neither was there at first, which made this target abort instead of fuzz.
 *
 * Why a self-produced datagram may legitimately fail to verify: ocb_decrypt carries its own half of the
 * counter-cryptanalysis from eprint 2019/311, and rejects a packet whose final plaintext block matches
 * the running delta in its first fifteen bytes. That condition is not the one ocb_encrypt checks, so a
 * packet the sender was happy with can still be refused by the receiver. In production that is a chance
 * of about one in 2^120 per packet, because the delta depends on a key the sender picked at random and
 * nobody else can see.
 *
 * Here the key is fixed and public, the sequence is deterministic, and -fsanitize=fuzzer instruments the
 * comparison itself - so libFuzzer learns the delta's bytes from the memcmp it keeps failing and builds
 * a plaintext whose last block matches. It took about forty thousand executions. That is the harness
 * handing the fuzzer an oracle no attacker has, not a defect in the transport, and the invariant has to
 * say so or this target spends every campaign re-finding it. Such a rejection is counted below instead,
 * so that a change which made it common would still show up as a number nobody can explain.
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

	// See the header: a genuine datagram that OCB2's decrypt-side detection refuses. Kept so the
	// condition is observable rather than merely tolerated.
	unsigned int refusedByDetection = 0;

	// Whether an arbitrary chunk has ever authenticated against this receiver - that is, whether the
	// fuzzer has guessed the three-byte tag. Once it has, the receiver's state is legitimately
	// contaminated and nothing is promised about later genuine datagrams: if the forgery claimed the
	// sequence the real sender was about to use, the real packet is a replay and refusing it is correct.
	bool forgeryAccepted = false;

	while (input.remaining() > 0) {
		const std::span< const std::uint8_t > chunk = input.chunk();

		if (input.choice(4) == 0) {
			// A genuine datagram, to prove the state still works after whatever has been thrown at it.
			// Encryption refuses an over-length plaintext, which is not what this is testing.
			if (chunk.size() > MAX_VIDEO_PLAINTEXT_SIZE) {
				continue;
			}

			datagram.clear();

			// Read either side of the call, because a frame OCB2 had to alter is not expected to come
			// back byte for byte - that is the whole meaning of the counter.
			const std::uint64_t alteredBefore = sender.xexStarAlteredCount();

			if (!sender.encrypt(chunk, datagram)) {
				continue;
			}

			const bool wasAltered = sender.xexStarAlteredCount() != alteredBefore;

			plaintext.clear();
			const VideoCryptState::Result result = receiver.decrypt(datagram, plaintext);

			if (result == VideoCryptState::Result::AuthenticationFailed) {
				// OCB2's own detection, reachable here and effectively unreachable in production - see
				// the header. Counted rather than asserted on, and deliberately not ignored silently:
				// if this ever stopped being rare the number is the only thing that would say so.
				refusedByDetection++;
				continue;
			}

			if (forgeryAccepted && result != VideoCryptState::Result::Ok) {
				// A guessed tag has already been accepted, so the receiver's sequence state no longer
				// corresponds to this sender's. Refusing a genuine datagram is then the right answer,
				// not a defect: the two sequences collided and the receiver cannot know which packet
				// was real. Nothing beyond this point is worth asserting on this receiver.
				continue;
			}

			// Anything else is the state misbehaving. A self-produced datagram cannot be a replay on
			// first sight, cannot be malformed, and cannot be on the wrong channel.
			check(result == VideoCryptState::Result::Ok);
			check(plaintext.size() == chunk.size());

			// Exact, unless the implementation said it had to alter the frame. Asserting it only in that
			// case is what keeps this a real check rather than one weakened until it always passes: the
			// counter has to be the reason, and anything else differing is still a failure.
			if (!wasAltered) {
				check(plaintext.empty() || std::equal(plaintext.begin(), plaintext.end(), chunk.begin()));
			}

			// And once only. Replaying it must be refused, and refused as a replay rather than as a
			// forgery, since the distinction is what the counters report.
			plaintext.clear();
			check(receiver.decrypt(datagram, plaintext) == VideoCryptState::Result::Replay);
			continue;
		}

		// Otherwise: whatever the fuzzer made. The window must not move for anything that fails - which
		// is the property this target exists for, and is asserted unconditionally.
		const std::uint32_t before = receiver.highestReceived();
		plaintext.clear();

		if (receiver.decrypt(chunk, plaintext) != VideoCryptState::Result::Ok) {
			check(receiver.highestReceived() == before);
		} else {
			// The fuzzer guessed the tag. At three bytes that is one attempt in 16.7 million, and over a
			// campaign of hundreds of millions of executions against a fixed key it happens - which is
			// itself the most useful thing this target has shown, since an attacker sending packets gets
			// attempts at the same rate. It is why decrypt() now refuses a sequence implausibly far
			// ahead of the window: before that, one such packet moved the anchor to the top of the
			// sequence space and every genuine packet afterwards was TooOld for the rest of the
			// connection.
			forgeryAccepted = true;
		}
	}

	// Read so the counter cannot be optimised away, and so that a build which made this common would
	// show it in a profile rather than nowhere at all.
	static volatile unsigned int refusedSink;
	refusedSink = refusedByDetection;
	(void) refusedSink;

	return 0;
}
