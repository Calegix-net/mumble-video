// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

/*
 * Writes the fuzzers' seed corpus, using the real senders to produce it.
 *
 * A fuzzer handed random bytes spends its whole budget failing the Protobuf parse and never reaches the
 * reassembler at all. Handed a packet the real fragmenter made, every mutation it tries is one field of
 * a valid packet away from valid, which is where the reassembler's own arithmetic lives. The same goes
 * for the crypt state: a seed it produced itself verifies, so a mutation of one is a near-miss rather
 * than noise.
 *
 * So the seeds are generated rather than committed by hand, by the code under test, and regenerating
 * them is one command. Run it with the corpus directory as its argument:
 *
 *     MumbleFuzzMakeSeeds src/mumble-core/fuzz/corpus
 *
 * Deliberately deterministic: the same seeds every time, so regenerating produces no diff unless a
 * wire format actually changed. Inputs a fuzzer found later are added to the same directories by hand
 * and are not touched by this.
 */

#include "MediaDemux.h"
#include "VideoFragmentation.h"
#include "VideoTransport.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace Mumble::Protocol;

namespace {

/// The reassembly target reads its input as length-prefixed packets with a session and a clock step in
/// front of each, so a seed has to be written in that shape rather than as a bare packet. See
/// FuzzInput.
void appendScripted(std::vector< std::uint8_t > &out, std::uint8_t session, std::uint8_t clockStep,
					const std::vector< std::uint8_t > &packet, std::uint8_t maintenance) {
	out.push_back(session);
	out.push_back(clockStep);
	out.push_back(static_cast< std::uint8_t >(packet.size() >> 8));
	out.push_back(static_cast< std::uint8_t >(packet.size() & 0xff));
	out.insert(out.end(), packet.begin(), packet.end());
	out.push_back(maintenance);
}

bool write(const std::string &directory, const char *name, const std::vector< std::uint8_t > &bytes) {
	const std::string path = directory + "/" + name;
	std::FILE *file        = std::fopen(path.c_str(), "wb");

	if (!file) {
		std::fprintf(stderr, "make-seeds: cannot write %s\n", path.c_str());
		return false;
	}

	const bool ok = bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
	std::fclose(file);

	return ok;
}

/// A payload with structure rather than one repeated byte, so that a seed exercising reassembly order
/// has distinguishable fragments.
std::vector< std::uint8_t > codedBytes(std::size_t size) {
	std::vector< std::uint8_t > payload(size);

	for (std::size_t i = 0; i < size; ++i) {
		payload[i] = static_cast< std::uint8_t >(i * 31 + 7);
	}

	return payload;
}

VideoUnitHeader header(std::uint32_t session, std::uint64_t frame, std::uint32_t unit, bool keyframe) {
	VideoUnitHeader out;
	out.senderSession        = session;
	out.streamID             = 1;
	out.frameNumber          = frame;
	out.unitID               = unit;
	out.isKeyframe           = keyframe;
	out.isFrameEnd           = true;
	out.captureTimestampUsec = frame * 33333;
	out.width                = 1920;
	out.height               = 1080;

	return out;
}

bool makeReassemblySeeds(const std::string &directory) {
	VideoFragmenter fragmenter;
	bool ok = true;

	// One packet of a one-fragment unit: the smallest thing that reaches the reassembler and completes.
	if (fragmenter.fragment(header(1, 1, 0, true), codedBytes(64))) {
		std::vector< std::uint8_t > seed;
		appendScripted(seed, 0, 1, fragmenter.packets().front(), 0);
		ok = write(directory, "reassembly-single", seed) && ok;
	}

	// A unit large enough to need many fragments, delivered in order: the path a healthy keyframe takes.
	if (fragmenter.fragment(header(1, 2, 0, true), codedBytes(VideoFragmenter::maxPayloadPerFragment() * 6 + 11))) {
		std::vector< std::uint8_t > inOrder;
		std::vector< std::uint8_t > reversed;
		const std::vector< std::vector< byte > > &packets = fragmenter.packets();

		for (std::size_t i = 0; i < packets.size(); ++i) {
			appendScripted(inOrder, 0, 1, packets[i], 0);
			appendScripted(reversed, 0, 1, packets[packets.size() - 1 - i], 0);
		}

		ok = write(directory, "reassembly-in-order", inOrder) && ok;
		// And out of order, which is the case the holding and the NACK accounting exist for.
		ok = write(directory, "reassembly-reversed", reversed) && ok;

		// The same unit missing its middle fragment, then asked about: this is the shape that reaches
		// collectNacks() with something to say.
		std::vector< std::uint8_t > gapped;

		for (std::size_t i = 0; i < packets.size(); ++i) {
			if (i == packets.size() / 2) {
				continue;
			}

			appendScripted(gapped, 0, 20, packets[i], 2);
		}

		ok = write(directory, "reassembly-gap-then-nack", gapped) && ok;
	}

	// Two senders interleaved, which is what keys the per-sender limits apart.
	std::vector< std::uint8_t > twoSenders;

	for (std::uint32_t session = 1; session <= 2; ++session) {
		if (fragmenter.fragment(header(session, session, 0, true), codedBytes(200))) {
			appendScripted(twoSenders, static_cast< std::uint8_t >(session - 1), 1, fragmenter.packets().front(), 0);
		}
	}

	ok = write(directory, "reassembly-two-senders", twoSenders) && ok;

	// A packet whose claimed sender contradicts the one the caller supplies. Rejected, and worth a seed
	// because that rejection is a security property rather than a formatting one.
	if (fragmenter.fragment(header(9, 3, 0, true), codedBytes(32))) {
		std::vector< std::uint8_t > spoofed;
		appendScripted(spoofed, 0, 1, fragmenter.packets().front(), 0);
		ok = write(directory, "reassembly-session-mismatch", spoofed) && ok;
	}

	return ok;
}

bool makeCryptSeeds(const std::string &directory) {
	// The same fixed key the target uses, so a seed it produced verifies there too.
	VideoCryptState sender;

	if (!sender.setKey(std::string(16, '\x2a'), std::string(12, '\x02'), std::string(12, '\x01'))) {
		std::fprintf(stderr, "make-seeds: the fuzz key was refused\n");
		return false;
	}

	bool ok = true;
	std::vector< std::uint8_t > datagram;
	std::vector< std::uint8_t > seed;

	// Three genuine datagrams of different sizes. The target's own "encrypt this chunk" branch needs the
	// plaintext, not the datagram, so the seed carries plaintexts and lets the target produce the
	// datagrams - which is also what keeps the seed valid if the framing ever changes.
	for (const std::size_t size : { std::size_t(0), std::size_t(37), MAX_VIDEO_PLAINTEXT_SIZE }) {
		const std::vector< std::uint8_t > plaintext = codedBytes(size);
		seed.push_back(static_cast< std::uint8_t >(size >> 8));
		seed.push_back(static_cast< std::uint8_t >(size & 0xff));
		seed.insert(seed.end(), plaintext.begin(), plaintext.end());
		// Zero selects the genuine-datagram branch in the target.
		seed.push_back(0);
	}

	ok = write(directory, "crypt-genuine", seed) && ok;

	// One real datagram as bytes, so that mutating a tag or a sequence number is one bit-flip away from
	// a packet that would have verified - the most productive thing a fuzzer can be given here.
	if (sender.encrypt(codedBytes(64), datagram)) {
		std::vector< std::uint8_t > forged;
		forged.push_back(static_cast< std::uint8_t >(datagram.size() >> 8));
		forged.push_back(static_cast< std::uint8_t >(datagram.size() & 0xff));
		forged.insert(forged.end(), datagram.begin(), datagram.end());
		// Non-zero selects the "whatever the fuzzer made" branch.
		forged.push_back(1);
		ok = write(directory, "crypt-real-datagram", forged) && ok;
	}

	return ok;
}

bool makeDemuxSeeds(const std::string &directory) {
	bool ok = true;

	// One datagram per channel byte the demux knows, plus an empty one and a byte on its own, which are
	// the two inputs that have historically been got wrong.
	ok = write(directory, "demux-empty", {}) && ok;
	ok = write(directory, "demux-channel-only", { MEDIA_CHANNEL_VIDEO }) && ok;

	for (const std::uint8_t channel : { MEDIA_CHANNEL_AUDIO, MEDIA_CHANNEL_VIDEO, std::uint8_t(0x7f) }) {
		std::vector< std::uint8_t > seed = { channel };
		const std::vector< std::uint8_t > body = codedBytes(48);
		seed.insert(seed.end(), body.begin(), body.end());

		char name[32];
		std::snprintf(name, sizeof(name), "demux-channel-%02x", channel);
		ok = write(directory, name, seed) && ok;
	}

	return ok;
}

/// The receive target encrypts the plaintexts in its seed itself, so a seed is the same shape as the
/// crypt target's: a length-prefixed plaintext, a clock step, and a byte choosing whether to corrupt
/// the datagram afterwards. Real video packets, so the parse behind the decryption is reached.
bool makeReceiveSeeds(const std::string &directory) {
	VideoFragmenter fragmenter;
	std::vector< std::uint8_t > seed;

	// A keyframe large enough to be fragmented, then a few small frames after it: the ordinary shape of
	// a stream starting, which is what the peek parse and the reassembler see together.
	for (std::uint64_t frame = 1; frame <= 4; ++frame) {
		const bool keyframe   = frame == 1;
		const std::size_t big = VideoFragmenter::maxPayloadPerFragment() * 4 + 5;

		if (!fragmenter.fragment(header(1, frame, 0, keyframe), codedBytes(keyframe ? big : 300))) {
			continue;
		}

		for (const std::vector< byte > &packet : fragmenter.packets()) {
			seed.push_back(static_cast< std::uint8_t >(packet.size() >> 8));
			seed.push_back(static_cast< std::uint8_t >(packet.size() & 0xff));
			seed.insert(seed.end(), packet.begin(), packet.end());
			// A 5 ms clock step, so the retransmission interval is crossed every other packet.
			seed.push_back(5);
			// Non-zero: do not corrupt. A seed that is refused by the tag reaches nothing.
			seed.push_back(1);
		}
	}

	return write(directory, "receive-stream-start", seed);
}

} // namespace

int main(int argc, char **argv) {
	if (argc != 2) {
		std::fprintf(stderr, "usage: %s <corpus-directory>\n", argv[0]);
		return 2;
	}

	const std::string root = argv[1];
	const bool ok = makeReassemblySeeds(root + "/reassembly") && makeCryptSeeds(root + "/crypt")
					&& makeDemuxSeeds(root + "/demux") && makeReceiveSeeds(root + "/receive");

	if (!ok) {
		std::fprintf(stderr, "make-seeds: failed; do the corpus directories exist?\n");
		return 1;
	}

	std::printf("make-seeds: wrote the seed corpus under %s\n", root.c_str());

	return 0;
}
