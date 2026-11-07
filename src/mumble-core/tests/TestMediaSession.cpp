// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "CoreTest.h"

#include "ControlFramer.h"
#include "CoreLog.h"
#include "MediaDemux.h"
#include "Mumble.pb.h"
#include "MumbleProtocol.h"
#include "MumbleUDP.pb.h"
#include "VideoFragmentation.h"
#include "VideoReceiver.h"
#include "VideoTransport.h"
#include "crypto/CryptStateOCB2.h"
#include "mumble-core/MumbleCore.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace Mumble::Protocol;

namespace {

using Bytes = std::vector< std::uint8_t >;

const std::string SESSION_KEY = "0123456789abcdef";

// A unit as the server relays it: fragmented by the sender, stamped with its session, encrypted for us.
std::vector< Bytes > relayedUnit(VideoCryptState &serverCrypt, std::uint32_t sender, std::uint32_t stream,
								 std::uint64_t frame, std::size_t payloadSize) {
	VideoUnitHeader header;
	header.senderSession = sender;
	header.streamID      = stream;
	header.frameNumber   = frame;
	header.isKeyframe    = true;
	header.isFrameEnd    = true;
	header.width         = 64;
	header.height        = 32;

	Bytes payload(payloadSize);
	for (std::size_t i = 0; i < payload.size(); i++) {
		payload[i] = static_cast< std::uint8_t >(i * 7);
	}

	VideoFragmenter fragmenter;
	if (!fragmenter.fragment(header, payload)) {
		return {};
	}

	std::vector< Bytes > datagrams;
	for (const std::vector< byte > &packet : fragmenter.packets()) {
		Bytes datagram;
		serverCrypt.encrypt(packet, datagram);
		datagrams.push_back(std::move(datagram));
	}
	return datagrams;
}

} // namespace

CORE_TEST(demuxSplitsByChannelByte) {
	const MediaDemux demux;
	const Bytes audio = { MEDIA_CHANNEL_AUDIO, 1, 2, 3, 4 };
	const Bytes video = { MEDIA_CHANNEL_VIDEO, 1, 2, 3, 4, 5 };
	const Bytes other = { 0x7F, 1, 2, 3, 4 };
	const Bytes runt  = { MEDIA_CHANNEL_AUDIO, 1, 2, 3 };

	const MediaDatagram a = demux.classify(audio);
	CHECK(a.channel == MediaChannel::Audio);
	CHECK_EQ(a.payload.size(), 4u);
	CHECK(a.payload.data() == audio.data() + 1);

	const MediaDatagram v = demux.classify(video);
	CHECK(v.channel == MediaChannel::Video);
	CHECK_EQ(v.datagram.size(), video.size());

	CHECK(demux.classify(other).channel == MediaChannel::Unknown);
	CHECK(demux.classify(runt).channel == MediaChannel::Unknown);
}

CORE_TEST(demuxPlainFramingIsAllAudio) {
	const MediaDemux demux(MediaDemux::Framing::Plain);
	const Bytes datagram = { MEDIA_CHANNEL_VIDEO, 1, 2, 3, 4 };

	const MediaDatagram d = demux.classify(datagram);
	CHECK(d.channel == MediaChannel::Audio);
	CHECK_EQ(d.payload.size(), datagram.size());
}

CORE_TEST(controlHeaderLayout) {
	const Bytes payload = { 0xAA, 0xBB, 0xCC };
	const Bytes framed  = ControlFramer::frame(static_cast< std::uint16_t >(TCPMessageType::CryptSetup), payload);
	CHECK(framed == (Bytes{ 0x00, 0x0F, 0x00, 0x00, 0x00, 0x03, 0xAA, 0xBB, 0xCC }));

	const ControlHeader header = readControlHeader(framed.data());
	CHECK_EQ(header.type, 15);
	CHECK_EQ(header.length, 3u);
}

CORE_TEST(controlFramerReassemblesAnySplit) {
	Bytes stream;
	for (std::uint16_t type = 0; type < 5; type++) {
		const Bytes payload(type * 100u, static_cast< std::uint8_t >(type));
		const Bytes framed = ControlFramer::frame(type, payload);
		stream.insert(stream.end(), framed.begin(), framed.end());
	}

	for (const std::size_t chunk : { std::size_t{ 1 }, std::size_t{ 5 }, std::size_t{ 7 }, stream.size() }) {
		ControlFramer framer;
		for (std::size_t i = 0; i < stream.size(); i += chunk) {
			CHECK(framer.feed(std::span< const std::uint8_t >(stream).subspan(i, std::min(chunk, stream.size() - i))));
		}
		for (std::uint16_t type = 0; type < 5; type++) {
			const std::optional< ControlMessage > message = framer.next();
			CHECK(message.has_value());
			CHECK_EQ(message->type, type);
			CHECK(message->payload == Bytes(type * 100u, static_cast< std::uint8_t >(type)));
		}
		CHECK(!framer.next().has_value());
	}
}

CORE_TEST(controlFramerRefusesOversizedMessages) {
	ControlFramer framer;
	Bytes header(CONTROL_HEADER_SIZE);
	writeControlHeader(header.data(), 1, MAX_CONTROL_PAYLOAD_SIZE + 1);
	CHECK(!framer.feed(header));
	CHECK(framer.failed());
	CHECK(!framer.feed(ControlFramer::frame(1, Bytes{ 1 })));
	CHECK(!framer.next().has_value());
}

CORE_TEST(videoReceiverReassemblesRelayedUnits) {
	VideoCryptState server;
	VideoCryptState client;
	CHECK(server.deriveFromSessionKey(SESSION_KEY, true));
	CHECK(client.deriveFromSessionKey(SESSION_KEY, false));

	VideoReceiver receiver;
	VideoReceiveEvents events;
	std::uint64_t now = 1000;
	for (const Bytes &datagram : relayedUnit(server, 7, 2, 40, 5000)) {
		receiver.receive(client, datagram, now++, events);
	}

	CHECK_EQ(events.units.size(), 1u);
	CHECK_EQ(events.units[0].header.senderSession, 7u);
	CHECK_EQ(events.units[0].header.streamID, 2u);
	CHECK_EQ(events.units[0].header.frameNumber, 40u);
	CHECK_EQ(events.units[0].payload.size(), 5000u);
	CHECK(events.packetsToSend.empty());
}

CORE_TEST(videoReceiverDropsUnstampedAndForeignPackets) {
	VideoCryptState server;
	VideoCryptState client;
	CHECK(server.deriveFromSessionKey(SESSION_KEY, true));
	CHECK(client.deriveFromSessionKey(SESSION_KEY, false));

	VideoReceiver receiver;
	VideoReceiveEvents events;
	for (const Bytes &datagram : relayedUnit(server, 0, 1, 1, 100)) {
		receiver.receive(client, datagram, 1000, events);
	}
	CHECK(events.units.empty());

	// Encrypted for the server, not for us.
	VideoCryptState otherDirection;
	CHECK(otherDirection.deriveFromSessionKey(SESSION_KEY, false));
	for (const Bytes &datagram : relayedUnit(otherDirection, 7, 1, 1, 100)) {
		receiver.receive(client, datagram, 1000, events);
	}
	CHECK(events.units.empty());
	CHECK_EQ(receiver.reassembler().pendingUnitCount(), 0u);
}

CORE_TEST(videoReceiverPassesOnRetransmissionRequests) {
	VideoCryptState server;
	VideoCryptState client;
	CHECK(server.deriveFromSessionKey(SESSION_KEY, true));
	CHECK(client.deriveFromSessionKey(SESSION_KEY, false));

	MumbleUDP::VideoNack nack;
	nack.set_sender_session(3);
	nack.set_stream_id(1);
	nack.add_units()->set_frame_number(99);
	Bytes datagram;
	CHECK(server.encrypt(encodeVideoNackPacket(nack), datagram));

	VideoReceiver receiver;
	VideoReceiveEvents events;
	receiver.receive(client, datagram, 1000, events);
	CHECK_EQ(events.nacksForUs.size(), 1u);

	MumbleUDP::VideoNack parsed;
	CHECK(parsed.ParseFromArray(events.nacksForUs[0].data(), static_cast< int >(events.nacksForUs[0].size())));
	CHECK_EQ(parsed.units(0).frame_number(), 99u);
}

CORE_TEST(videoReceiverAsksForMissingFragments) {
	VideoCryptState server;
	VideoCryptState client;
	CHECK(server.deriveFromSessionKey(SESSION_KEY, true));
	CHECK(client.deriveFromSessionKey(SESSION_KEY, false));

	std::vector< Bytes > unit = relayedUnit(server, 7, 2, 40, 5000);
	CHECK(unit.size() > 2);
	unit.erase(unit.begin() + 1);

	VideoReceiver receiver;
	VideoReceiveEvents events;
	std::uint64_t now = 1'000'000;
	for (const Bytes &datagram : unit) {
		receiver.receive(client, datagram, now, events);
	}
	CHECK(events.units.empty());
	CHECK(events.packetsToSend.empty());

	// Nothing is asked for until the unit has been incomplete for VIDEO_NACK_DELAY_USEC, and the pass only
	// runs as packets arrive: here, a later unit.
	now += VIDEO_NACK_DELAY_USEC + VideoReceiver::NACK_COLLECT_INTERVAL_USEC;
	for (const Bytes &datagram : relayedUnit(server, 7, 2, 41, 100)) {
		receiver.receive(client, datagram, now, events);
	}
	CHECK(events.collectedNacks);
	CHECK_EQ(events.unitsRequested, 1u);
	CHECK_EQ(events.packetsToSend.size(), 1u);

	const Bytes &packet = events.packetsToSend[0];
	CHECK_EQ(packet[0], static_cast< std::uint8_t >(UDPMessageType::VideoNack));
	MumbleUDP::VideoNack nack;
	CHECK(nack.ParseFromArray(packet.data() + 1, static_cast< int >(packet.size() - 1)));
	CHECK_EQ(nack.sender_session(), 7u);
	CHECK_EQ(nack.stream_id(), 2u);
	CHECK_EQ(nack.units_size(), 1);
	CHECK_EQ(nack.units(0).frame_number(), 40u);
	CHECK_EQ(nack.units(0).missing_fragments(), std::uint64_t{ 1 } << 1);
}

CORE_TEST(videoReceiverPacesRetransmissionRequests) {
	VideoCryptState server;
	VideoCryptState client;
	CHECK(server.deriveFromSessionKey(SESSION_KEY, true));
	CHECK(client.deriveFromSessionKey(SESSION_KEY, false));

	// One incomplete unit on each of more streams than may be asked about in a second.
	VideoReceiver receiver;
	VideoReceiveEvents events;
	std::uint64_t now = 1'000'000;
	for (std::uint32_t stream = 1; stream <= VideoReceiver::MAX_NACK_MESSAGES_PER_SECOND + 10; stream++) {
		std::vector< Bytes > unit = relayedUnit(server, 7, stream, 1, 3000);
		receiver.receive(client, unit[0], now, events);
	}

	now += VIDEO_NACK_DELAY_USEC + VideoReceiver::NACK_COLLECT_INTERVAL_USEC;
	VideoReceiveEvents later;
	receiver.receive(client, relayedUnit(server, 7, 999, 1, 3000)[0], now, later);
	CHECK_EQ(later.unitsRequested, VideoReceiver::MAX_NACK_MESSAGES_PER_SECOND + 10);
	CHECK_EQ(later.packetsToSend.size(), VideoReceiver::MAX_NACK_MESSAGES_PER_SECOND);
}

// Re-delivering the session key must not restart the sequence counter.
//
// The nonce is the derived base followed by the sequence, and the base is a pure function of the key, so
// a restart under an unchanged key would encrypt new frames under nonces already spent. For OCB that is
// not a weakened mode, it is a broken one. The client re-derives on any CryptSetup carrying a key, so
// this is reachable from the wire rather than hypothetical.
CORE_TEST(reKeyingWithTheSameKeyKeepsTheSequence) {
	VideoCryptState sender;
	CHECK(sender.deriveFromSessionKey(SESSION_KEY, false));
	CHECK_EQ(sender.nextSequence(), VIDEO_FIRST_SEQUENCE);

	const Bytes plaintext = { static_cast< byte >(UDPMessageType::Video), 0x08, 0x01 };
	Bytes first;
	for (int i = 0; i < 10; i++) {
		CHECK(sender.encrypt(plaintext, first));
	}

	const std::uint32_t reached = sender.nextSequence();
	CHECK_EQ(reached, VIDEO_FIRST_SEQUENCE + 10);

	// The same session key again, exactly as a repeated CryptSetup delivers it.
	CHECK(sender.deriveFromSessionKey(SESSION_KEY, false));
	CHECK_EQ(sender.nextSequence(), reached);

	// And the nonce really is fresh: the next datagram carries a sequence no earlier datagram used.
	Bytes next;
	CHECK(sender.encrypt(plaintext, next));
	CHECK_EQ(next[VIDEO_SEQUENCE_OFFSET + 3], static_cast< byte >(reached & 0xFF));

	// A genuinely different key is a different nonce space, so there the restart is correct.
	CHECK(sender.deriveFromSessionKey("fedcba9876543210", false));
	CHECK_EQ(sender.nextSequence(), VIDEO_FIRST_SEQUENCE);
}

// A single packet at an implausible sequence must not end the stream.
//
// The replay window anchors to the highest sequence accepted. One forged packet carrying a sequence near
// the top of the 32-bit space used to move that anchor there, after which every genuine packet was
// TooOld for the rest of the connection - a permanent denial of video from one lucky packet, since the
// authentication tag is only three bytes wide. MumbleFuzzcrypt produced exactly that, which is what
// prompted this.
CORE_TEST(oneHugeSequenceDoesNotLockOutTheSender) {
	// A sender whose counter can be moved, which is what makes a forgery reproducible: the packet below
	// is genuine and authenticates, and carries a sequence no honest sender would reach.
	struct Settable : VideoCryptState {
		void setNextSequence(std::uint32_t seq) { m_nextSequence = seq; }
	};

	Settable sender;
	VideoCryptState receiver;
	CHECK(sender.deriveFromSessionKey(SESSION_KEY, true));
	CHECK(receiver.deriveFromSessionKey(SESSION_KEY, false));

	const Bytes plaintext = { static_cast< byte >(UDPMessageType::Video), 0x08, 0x01 };
	Bytes datagram;
	Bytes out;

	// Ordinary traffic first, so there is something to lock out.
	CHECK(sender.encrypt(plaintext, datagram));
	CHECK(receiver.decrypt(datagram, out) == VideoCryptState::Result::Ok);

	// Now one authentic packet from far away.
	sender.setNextSequence(0xFFFF0000u);
	CHECK(sender.encrypt(plaintext, datagram));
	CHECK(receiver.decrypt(datagram, out) == VideoCryptState::Result::TooFarAhead);
	CHECK_EQ(receiver.tooFarAheadCount(), 1u);
	CHECK_EQ(receiver.resyncCount(), 0u);

	// The anchor did not move, so the genuine stream carries on exactly as before.
	sender.setNextSequence(2);
	int accepted = 0;
	for (int i = 0; i < 50; i++) {
		CHECK(sender.encrypt(plaintext, datagram));
		if (receiver.decrypt(datagram, out) == VideoCryptState::Result::Ok) {
			accepted++;
		}
	}
	CHECK_EQ(accepted, 50);
}

// But a sender that really has moved on must be able to re-anchor, or the bound above would be a
// lockout of its own making: a long outage while the sender keeps encoding would end video for good.
CORE_TEST(aRunOfAuthenticPacketsReAnchorsTheWindow) {
	struct Settable : VideoCryptState {
		void setNextSequence(std::uint32_t seq) { m_nextSequence = seq; }
	};

	Settable sender;
	VideoCryptState receiver;
	CHECK(sender.deriveFromSessionKey(SESSION_KEY, true));
	CHECK(receiver.deriveFromSessionKey(SESSION_KEY, false));

	const Bytes plaintext = { static_cast< byte >(UDPMessageType::Video), 0x08, 0x01 };
	Bytes datagram;
	Bytes out;

	CHECK(sender.encrypt(plaintext, datagram));
	CHECK(receiver.decrypt(datagram, out) == VideoCryptState::Result::Ok);

	// Far beyond the jump bound, as after a long blackout.
	sender.setNextSequence(5'000'000);

	// The first VIDEO_RESYNC_PACKETS - 1 are refused, and say why.
	for (unsigned int i = 0; i < VIDEO_RESYNC_PACKETS - 1; i++) {
		CHECK(sender.encrypt(plaintext, datagram));
		CHECK(receiver.decrypt(datagram, out) == VideoCryptState::Result::TooFarAhead);
		CHECK_EQ(receiver.resyncCount(), 0u);
	}

	// The next one completes the run, and the window moves to it.
	CHECK(sender.encrypt(plaintext, datagram));
	CHECK(receiver.decrypt(datagram, out) == VideoCryptState::Result::Ok);
	CHECK_EQ(receiver.resyncCount(), 1u);
	CHECK_EQ(receiver.highestReceived(), 5'000'000u + VIDEO_RESYNC_PACKETS - 1);

	// And it is a working connection again, not a one-off acceptance.
	for (int i = 0; i < 20; i++) {
		CHECK(sender.encrypt(plaintext, datagram));
		CHECK(receiver.decrypt(datagram, out) == VideoCryptState::Result::Ok);
	}
}

// The run has to be consecutive. Anything accepted normally in between resets it, which is what stops a
// forger accumulating a re-anchor one lucky packet at a time while a stream is running.
CORE_TEST(anInWindowPacketResetsTheResyncRun) {
	struct Settable : VideoCryptState {
		void setNextSequence(std::uint32_t seq) { m_nextSequence = seq; }
	};

	Settable sender;
	VideoCryptState receiver;
	CHECK(sender.deriveFromSessionKey(SESSION_KEY, true));
	CHECK(receiver.deriveFromSessionKey(SESSION_KEY, false));

	const Bytes plaintext = { static_cast< byte >(UDPMessageType::Video), 0x08, 0x01 };
	Bytes datagram;
	Bytes out;
	std::uint32_t ordinary = VIDEO_FIRST_SEQUENCE;

	// Alternate: one from far away, one ordinary, over and over. Far more than VIDEO_RESYNC_PACKETS of
	// the former, but never two in a row, so the window must never re-anchor.
	for (unsigned int i = 0; i < VIDEO_RESYNC_PACKETS * 4; i++) {
		sender.setNextSequence(9'000'000 + i);
		CHECK(sender.encrypt(plaintext, datagram));
		CHECK(receiver.decrypt(datagram, out) == VideoCryptState::Result::TooFarAhead);

		sender.setNextSequence(ordinary++);
		CHECK(sender.encrypt(plaintext, datagram));
		CHECK(receiver.decrypt(datagram, out) == VideoCryptState::Result::Ok);
	}

	CHECK_EQ(receiver.resyncCount(), 0u);
	CHECK(receiver.highestReceived() < 9'000'000u);
}

// A refused re-key must leave the state alone, sequence counter included.
//
// The refusal path used to clear m_valid before validating its arguments. That threw away the only
// record of which key was installed, so the next call with that same key looked like a fresh key and
// restarted the sequence - spending nonces a second time under a key that had already used them, which
// is the break the test above this one exists to prevent. Found by review rather than by the fuzzer,
// which cannot call setKey directly.
CORE_TEST(aRefusedReKeyChangesNothing) {
	const std::string key(AES_KEY_SIZE_BYTES, '\x2b');
	const std::string baseA(12, '\x01');
	const std::string baseB(12, '\x02');

	VideoCryptState state;
	CHECK(state.setKey(key, baseA, baseB));

	const Bytes plaintext = { static_cast< byte >(UDPMessageType::Video), 0x08, 0x01 };
	Bytes datagram;
	for (int i = 0; i < 5; i++) {
		CHECK(state.encrypt(plaintext, datagram));
	}

	const std::uint32_t reached = state.nextSequence();
	CHECK_EQ(reached, VIDEO_FIRST_SEQUENCE + 5);

	// Refused three ways: equal nonce bases, a short key, a short base. None may disturb anything.
	CHECK(!state.setKey(key, baseA, baseA));
	CHECK(state.isValid());
	CHECK_EQ(state.nextSequence(), reached);

	CHECK(!state.setKey(std::string(4, '\x2b'), baseA, baseB));
	CHECK(state.isValid());
	CHECK_EQ(state.nextSequence(), reached);

	CHECK(!state.setKey(key, std::string(4, '\x01'), baseB));
	CHECK(state.isValid());
	CHECK_EQ(state.nextSequence(), reached);

	// And the key really is still the one that was installed: re-installing it is recognised as a
	// repeat, so the counter stays put rather than restarting.
	CHECK(state.setKey(key, baseA, baseB));
	CHECK_EQ(state.nextSequence(), reached);

	// It still encrypts, and with a sequence no earlier datagram used.
	CHECK(state.encrypt(plaintext, datagram));
	CHECK_EQ(datagram[VIDEO_SEQUENCE_OFFSET + 3], static_cast< byte >(reached & 0xFF));
}

// A plaintext OCB2 counter-cryptanalysis objects to is delivered, altered by exactly one bit, counted.
//
// ocb_encrypt flips one plaintext bit when the block before last is fifteen zero bytes, and folds the
// flip into the checksum so the tag verifies against what it sent rather than what it was given. The
// receiver therefore cannot detect it.
//
// This was briefly made a refusal instead, on the grounds that silent alteration is the wrong failure
// mode for a layer that exists to provide integrity. The tests disagreed: 64 zero bytes trip the
// condition, so three fixtures in TestVideoTransport stopped being sendable, and a payload with a zero
// run at the wrong offset is not an exotic thing for a flat region of a screen to produce. Refusing is
// deterministic for a given payload, so that region would have stopped updating for as long as it
// looked that way. So it is delivered, and the counter is what makes the alteration explicable.
CORE_TEST(videoCountsAFrameOcb2HadToAlter) {
	VideoCryptState sender;
	VideoCryptState receiver;
	CHECK(sender.deriveFromSessionKey(SESSION_KEY, true));
	CHECK(receiver.deriveFromSessionKey(SESSION_KEY, false));

	// 40 bytes, so ocb_encrypt examines the block starting at byte 16. Fifteen zeros there is the
	// condition; the first byte is the message type, which is why a short plaintext cannot trigger it.
	Bytes trigger(40, 0x5A);
	trigger[0] = static_cast< byte >(UDPMessageType::Video);
	for (std::size_t i = 16; i < 31; i++) {
		trigger[i] = 0;
	}

	Bytes datagram;
	CHECK_EQ(sender.xexStarAlteredCount(), 0u);
	CHECK(sender.encrypt(trigger, datagram));
	CHECK_EQ(sender.xexStarAlteredCount(), 1u);

	// It authenticates, because the tag was computed over what was actually sent.
	Bytes out;
	CHECK(receiver.decrypt(datagram, out) == VideoCryptState::Result::Ok);
	CHECK_EQ(out.size(), trigger.size());

	// And differs from the input by exactly one bit. Stated as a number rather than described, because
	// the whole point is that nothing downstream can tell.
	CHECK(out != trigger);
	int differingBits = 0;
	for (std::size_t i = 0; i < out.size(); i++) {
		for (int bit = 0; bit < 8; bit++) {
			if (((out[i] ^ trigger[i]) >> bit) & 1) {
				differingBits++;
			}
		}
	}
	CHECK_EQ(differingBits, 1);

	// An ordinary payload is untouched and does not move the counter.
	Bytes ordinary(40, 0x5A);
	ordinary[0] = static_cast< byte >(UDPMessageType::Video);
	CHECK(sender.encrypt(ordinary, datagram));
	CHECK_EQ(sender.xexStarAlteredCount(), 1u);
	CHECK(receiver.decrypt(datagram, out) == VideoCryptState::Result::Ok);
	CHECK(out == ordinary);
}


// The receive half of the same thing: a repeat must not clear the replay window, or every datagram the
// peer has already sent becomes acceptable again.
CORE_TEST(reKeyingWithTheSameKeyKeepsTheReplayWindow) {
	VideoCryptState server;
	VideoCryptState client;
	CHECK(server.deriveFromSessionKey(SESSION_KEY, true));
	CHECK(client.deriveFromSessionKey(SESSION_KEY, false));

	const Bytes plaintext = { static_cast< byte >(UDPMessageType::Video), 0x08, 0x01 };
	Bytes datagram;
	CHECK(server.encrypt(plaintext, datagram));

	Bytes out;
	CHECK(client.decrypt(datagram, out) == VideoCryptState::Result::Ok);
	CHECK(client.decrypt(datagram, out) == VideoCryptState::Result::Replay);

	// Re-key with the same key, then offer that same datagram once more.
	CHECK(client.deriveFromSessionKey(SESSION_KEY, false));
	CHECK(client.decrypt(datagram, out) == VideoCryptState::Result::Replay);

	// A different key resets the window, which is right - but it also cannot decrypt the old datagram.
	CHECK(client.deriveFromSessionKey("fedcba9876543210", false));
	CHECK_EQ(client.highestReceived(), 0u);
}

// The public API end to end, against a server played by the core's own server-side pieces.
CORE_TEST(mediaSessionTalksToAServer) {
	CryptStateOCB2 serverAudio;
	serverAudio.genKey();
	VideoCryptState serverVideo;
	CHECK(serverVideo.deriveFromSessionKey(serverAudio.getRawKey(), true));

	MumbleCore::MediaSession session;
	MumbleCore::MediaEvents events;
	CHECK(!session.isKeyed());

	MumbleProto::CryptSetup setup;
	setup.set_key(serverAudio.getRawKey());
	setup.set_client_nonce(serverAudio.getDecryptIV());
	setup.set_server_nonce(serverAudio.getEncryptIV());
	const std::string setupBytes = setup.SerializeAsString();
	CHECK(session.handleCryptSetup(
		std::span< const std::uint8_t >(reinterpret_cast< const std::uint8_t * >(setupBytes.data()), setupBytes.size()),
		events));
	CHECK(session.isKeyed());

	// Audio from another user.
	const Bytes opus = { 0xF8, 0x01, 0x02, 0x03 };
	AudioData audio;
	audio.senderSession = 5;
	audio.frameNumber   = 77;
	audio.payload       = opus;
	UDPAudioEncoder< Role::Server > audioEncoder(PROTOBUF_INTRODUCTION_VERSION);
	const std::span< const byte > audioPacket = audioEncoder.encodeAudioPacket(audio);
	Bytes datagram(audioPacket.size() + 5);
	datagram[0] = MEDIA_CHANNEL_AUDIO;
	CHECK(
		serverAudio.encrypt(audioPacket.data(), datagram.data() + 1, static_cast< unsigned int >(audioPacket.size())));
	session.receiveDatagram(datagram, 1000, events);
	CHECK_EQ(events.audio.size(), 1u);
	CHECK_EQ(events.audio[0].senderSession, 5u);
	CHECK_EQ(events.audio[0].frameNumber, 77u);
	CHECK(events.audio[0].opus == opus);

	// Our audio, as the server would read it.
	const std::optional< MumbleCore::Bytes > ours = session.makeAudio(opus, 12, 0, true);
	CHECK(ours.has_value());
	CHECK_EQ((*ours)[0], MEDIA_CHANNEL_AUDIO);
	UDPDecoder< Role::Server > serverDecoder(PROTOBUF_INTRODUCTION_VERSION);
	const std::span< byte > buffer = serverDecoder.getBuffer();
	CHECK(serverAudio.decrypt(ours->data() + 1, buffer.data(), static_cast< unsigned int >(ours->size() - 1)));
	CHECK(serverDecoder.decode(buffer.subspan(0, ours->size() - 5)));
	CHECK(serverDecoder.getMessageType() == UDPMessageType::Audio);
	CHECK_EQ(serverDecoder.getAudioData().frameNumber, 12u);
	CHECK(serverDecoder.getAudioData().isLastFrame);

	// A ping, echoed.
	const std::optional< MumbleCore::Bytes > ping = session.makePing(0xABCDEF);
	CHECK(ping.has_value());
	CHECK(serverAudio.decrypt(ping->data() + 1, buffer.data(), static_cast< unsigned int >(ping->size() - 1)));
	CHECK(serverDecoder.decode(buffer.subspan(0, ping->size() - 5)));
	CHECK(serverDecoder.getMessageType() == UDPMessageType::Ping);
	UDPPingEncoder< Role::Server > pingEncoder(PROTOBUF_INTRODUCTION_VERSION);
	const std::span< const byte > pong = pingEncoder.encodePingPacket(serverDecoder.getPingData());
	Bytes pongDatagram(pong.size() + 5);
	pongDatagram[0] = MEDIA_CHANNEL_AUDIO;
	CHECK(serverAudio.encrypt(pong.data(), pongDatagram.data() + 1, static_cast< unsigned int >(pong.size())));
	session.receiveDatagram(pongDatagram, 2000, events);
	CHECK_EQ(events.pongs.size(), 1u);
	CHECK_EQ(events.pongs[0], 0xABCDEFu);

	// Video, keyed from the same CryptSetup.
	for (const Bytes &videoDatagram : relayedUnit(serverVideo, 9, 1, 3, 4000)) {
		session.receiveDatagram(videoDatagram, 3000, events);
	}
	CHECK_EQ(events.video.size(), 1u);
	CHECK_EQ(events.video[0].senderSession, 9u);
	CHECK_EQ(events.video[0].payload.size(), 4000u);
	CHECK(events.video[0].isKeyframe);

	CHECK(events.datagramsToSend.empty());
	CHECK(events.controlToSend.empty());
	CHECK_EQ(session.stats().audioGood, 2u);
}

CORE_TEST(mediaSessionAnswersANonceRequest) {
	CryptStateOCB2 serverAudio;
	serverAudio.genKey();

	MumbleCore::MediaSession session;
	MumbleCore::MediaEvents events;

	MumbleProto::CryptSetup setup;
	setup.set_key(serverAudio.getRawKey());
	setup.set_client_nonce(serverAudio.getDecryptIV());
	setup.set_server_nonce(serverAudio.getEncryptIV());
	const std::string setupBytes = setup.SerializeAsString();
	CHECK(session.handleCryptSetup(
		std::span< const std::uint8_t >(reinterpret_cast< const std::uint8_t * >(setupBytes.data()), setupBytes.size()),
		events));

	// An empty CryptSetup asks for our nonce.
	CHECK(session.handleCryptSetup({}, events));
	CHECK_EQ(events.controlToSend.size(), 1u);

	MumbleCore::ControlReader reader;
	CHECK(reader.feed(events.controlToSend[0]));
	const std::optional< MumbleCore::ControlMessage > reply = reader.next();
	CHECK(reply.has_value());
	CHECK_EQ(reply->type, MumbleCore::ControlType::CryptSetup);
	MumbleProto::CryptSetup parsed;
	CHECK(parsed.ParseFromArray(reply->payload.data(), static_cast< int >(reply->payload.size())));
	CHECK_EQ(parsed.client_nonce(), serverAudio.getDecryptIV());
}

CORE_TEST(mediaSessionSpeaksPlainFraming) {
	CryptStateOCB2 serverAudio;
	serverAudio.genKey();

	MumbleCore::MediaSession session;
	MumbleCore::MediaEvents events;
	session.setUdpFraming(MumbleCore::UdpFraming::Plain);

	MumbleProto::CryptSetup setup;
	setup.set_key(serverAudio.getRawKey());
	setup.set_client_nonce(serverAudio.getDecryptIV());
	setup.set_server_nonce(serverAudio.getEncryptIV());
	const std::string setupBytes = setup.SerializeAsString();
	CHECK(session.handleCryptSetup(
		std::span< const std::uint8_t >(reinterpret_cast< const std::uint8_t * >(setupBytes.data()), setupBytes.size()),
		events));

	// No channel byte either way.
	const std::optional< MumbleCore::Bytes > ping = session.makePing(42);
	CHECK(ping.has_value());
	UDPDecoder< Role::Server > serverDecoder(PROTOBUF_INTRODUCTION_VERSION);
	const std::span< byte > buffer = serverDecoder.getBuffer();
	CHECK(serverAudio.decrypt(ping->data(), buffer.data(), static_cast< unsigned int >(ping->size())));
	CHECK(serverDecoder.decode(buffer.subspan(0, ping->size() - 4)));

	UDPPingEncoder< Role::Server > pingEncoder(PROTOBUF_INTRODUCTION_VERSION);
	const std::span< const byte > pong = pingEncoder.encodePingPacket(serverDecoder.getPingData());
	Bytes datagram(pong.size() + 4);
	CHECK(serverAudio.encrypt(pong.data(), datagram.data(), static_cast< unsigned int >(pong.size())));
	session.receiveDatagram(datagram, 1000, events);
	CHECK_EQ(events.pongs.size(), 1u);
	CHECK_EQ(events.pongs[0], 42u);
}

CORE_TEST(mediaSessionIgnoresRunts) {
	CryptStateOCB2 serverAudio;
	serverAudio.genKey();

	MumbleCore::MediaSession session;
	MumbleCore::MediaEvents events;
	MumbleProto::CryptSetup setup;
	setup.set_key(serverAudio.getRawKey());
	setup.set_client_nonce(serverAudio.getDecryptIV());
	setup.set_server_nonce(serverAudio.getEncryptIV());
	const std::string setupBytes = setup.SerializeAsString();
	CHECK(session.handleCryptSetup(
		std::span< const std::uint8_t >(reinterpret_cast< const std::uint8_t * >(setupBytes.data()), setupBytes.size()),
		events));

	for (const MumbleCore::UdpFraming framing :
		 { MumbleCore::UdpFraming::ChannelByte, MumbleCore::UdpFraming::Plain }) {
		session.setUdpFraming(framing);
		for (std::size_t size = 0; size < 6; size++) {
			const Bytes runt(size, 0);
			session.receiveDatagram(runt, 1000, events);
		}
	}
	CHECK(events.audio.empty());
	CHECK(events.pongs.empty());
}

namespace {
int g_publicLogCalls = 0;
void countPublicLog(MumbleCore::LogLevel level, const char *) {
	if (level == MumbleCore::LogLevel::Warning) {
		g_publicLogCalls++;
	}
}
} // namespace

CORE_TEST(publicLogHandlerGetsCoreMessages) {
	MumbleCore::setLogHandler(countPublicLog);
	Mumble::Core::logWarning("from the core");
	MumbleCore::setLogHandler(nullptr);
	CHECK_EQ(g_publicLogCalls, 1);
}
