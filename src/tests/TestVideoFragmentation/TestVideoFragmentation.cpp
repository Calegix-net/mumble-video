// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "MumbleProtocol.h"
#include "MumbleUDP.pb.h"
#include "VideoFragmentation.h"

#include <QObject>
#include <QtTest>

#include <cstdint>
#include <limits>
#include <vector>

using namespace Mumble::Protocol;

namespace {

/// What a fragment costs on the wire once the video transport wraps it: the header carrying the
/// channel byte, sequence number and authentication tag.
constexpr std::size_t WIRE_OVERHEAD = VIDEO_HEADER_SIZE;

/// The session the well-behaved fixtures below claim to be.
constexpr std::uint32_t SESSION = 42;

std::vector< byte > makePayload(std::size_t size) {
	std::vector< byte > payload(size);

	// A varying pattern, so that a fragment reassembled at the wrong offset does not accidentally
	// compare equal.
	for (std::size_t i = 0; i < size; ++i) {
		payload[i] = static_cast< byte >((i * 31 + (i >> 8)) & 0xFF);
	}

	return payload;
}

VideoUnitHeader makeHeader() {
	VideoUnitHeader header;
	header.senderSession        = SESSION;
	header.streamID             = 7;
	header.frameNumber          = 1234;
	header.unitID               = 3;
	header.isKeyframe           = true;
	header.isFrameEnd           = true;
	header.captureTimestampUsec = 987654321;
	header.x                    = 16;
	header.y                    = 32;
	header.width                = 640;
	header.height               = 480;

	return header;
}

/// Every field at its widest encoding. This is the only header that exercises the size budget at its
/// actual limit; the friendly fixture above leaves roughly fifty bytes of accidental slack.
VideoUnitHeader makeMaxHeader() {
	constexpr std::uint32_t MAX32 = std::numeric_limits< std::uint32_t >::max();
	constexpr std::uint64_t MAX64 = std::numeric_limits< std::uint64_t >::max();

	VideoUnitHeader header;
	header.senderSession        = MAX32;
	header.streamID             = MAX32;
	header.frameNumber          = MAX64;
	header.unitID               = MAX32;
	header.isKeyframe           = true;
	header.isFrameEnd           = true;
	header.captureTimestampUsec = MAX64;
	header.x                    = MAX32;
	header.y                    = MAX32;
	header.width                = MAX32;
	header.height               = MAX32;

	return header;
}

/// Serialises a hand-built message into a packet, for the cases that a well-behaved fragmenter would
/// never produce.
std::vector< byte > makePacket(const MumbleUDP::Video &message) {
	const std::size_t size = message.ByteSizeLong();

	std::vector< byte > packet(size + 1);
	packet[0] = static_cast< byte >(UDPMessageType::Video);
	message.SerializeToArray(packet.data() + 1, static_cast< int >(size));

	return packet;
}

/// Reproduces the append-style stamping that MumbleUDP.proto specifies for the server relay: field 1 as
/// a tag byte followed by a five-byte varint, appended without touching what is already there.
std::vector< byte > appendSenderSessionStamp(const std::vector< byte > &packet, std::uint32_t session) {
	std::vector< byte > stamped = packet;

	stamped.push_back(0x08);

	for (int i = 0; i < 4; ++i) {
		stamped.push_back(static_cast< byte >((session & 0x7F) | 0x80));
		session >>= 7;
	}

	stamped.push_back(static_cast< byte >(session & 0x7F));

	return stamped;
}

} // namespace

class TestVideoFragmentation : public QObject {
	Q_OBJECT
private slots:
	void budgetIsSane();
	void everyPacketFitsTheMTU();
	void worstCaseHeaderFitsTheMTU();
	void worstCaseSurvivesRelayStamping();
	void roundtripEmptyPayload();
	void roundtripSingleFragment();
	void roundtripMultiFragment();
	void roundtripAtMaxFragmentCount();
	void fragmentCountsAtBoundaries();
	void outOfOrderDeliveryReassembles();
	void duplicateFragmentsAreIgnored();
	void duplicateWithDifferentBytesKeepsTheFirst();
	void oversizedUnitIsRejected();
	void rejectsWrongMessageType();
	void rejectsTruncatedPacket();
	void rejectsMalformedProtobuf();
	void rejectsZeroFragmentCount();
	void rejectsExcessiveFragmentCount();
	void rejectsFragmentIndexOutOfRange();
	void rejectsInconsistentFragmentCount();
	void rejectsOversizedReassembledUnit();
	void rejectsSpoofedSenderSession();
	void spoofingCannotEvadeThePerSenderLimit();
	void spoofingCannotCorruptAnotherSendersUnit();
	void distinctSendersReassembleIndependently();
	void stalePartialUnitsExpire();
	void processPacketDrivesExpiry();
	void pendingUnitsAreCappedPerSender();
	void pendingUnitsAreCappedGlobally();
	void removeSenderDropsOnlyThatSender();
	void aBusySenderDoesNotEvictOtherSenders();
	void fragmenterReusesItsBuffers();
};

void TestVideoFragmentation::aBusySenderDoesNotEvictOtherSenders() {
	VideoReassembler reassembler;
	VideoUnit unit;

	auto openUnit = [&](std::uint32_t session, std::uint32_t frame, std::uint64_t now) {
		MumbleUDP::Video message;
		message.set_frame_number(frame);
		message.set_fragment_count(2);
		message.set_fragment_index(0);
		message.set_payload("x");

		return reassembler.processPacket(makePacket(message), session, now, unit);
	};

	// Fill the global pool with well-behaved senders, each well inside its own quota.
	const std::uint32_t quietSenders =
		static_cast< std::uint32_t >(MAX_PENDING_VIDEO_UNITS_TOTAL / MAX_PENDING_VIDEO_UNITS_PER_SENDER);

	std::uint64_t clock = 1;

	for (std::uint32_t session = 1; session <= quietSenders; ++session) {
		for (std::uint32_t frame = 0; frame < MAX_PENDING_VIDEO_UNITS_PER_SENDER; ++frame) {
			QCOMPARE(openUnit(session, frame, clock++), VideoReassemblyResult::Incomplete);
		}
	}

	const std::size_t filled = reassembler.pendingUnitCount();
	QVERIFY(filled > 0);

	// Now one sender churns through new units at speed. It is always at its own quota, so each new unit
	// should displace one of *its own*, never one belonging to somebody else. If the limits are applied
	// in the wrong order it evicts a stranger's unit first and the total collapses.
	for (std::uint32_t frame = 100; frame < 400; ++frame) {
		QCOMPARE(openUnit(1, frame, clock++), VideoReassemblyResult::Incomplete);
	}

	QCOMPARE(reassembler.pendingUnitCount(), filled);
}

void TestVideoFragmentation::fragmenterReusesItsBuffers() {
	VideoFragmenter fragmenter;

	const std::vector< byte > payload = makePayload(VideoFragmenter::maxPayloadPerFragment() * 3);

	QVERIFY(fragmenter.fragment(makeHeader(), payload));

	// Record where the packet buffers live, then fragment an identically shaped unit again. A reusing
	// implementation keeps the same allocations; one that clears and refills does not.
	std::vector< const void * > firstAddresses;

	for (const std::vector< byte > &packet : fragmenter.packets()) {
		firstAddresses.push_back(packet.data());
	}

	QVERIFY(fragmenter.fragment(makeHeader(), payload));
	QCOMPARE(fragmenter.packets().size(), firstAddresses.size());

	for (std::size_t i = 0; i < firstAddresses.size(); ++i) {
		QCOMPARE(fragmenter.packets()[i].data(), firstAddresses[i]);
	}
}

void TestVideoFragmentation::budgetIsSane() {
	const std::size_t maxPayload = VideoFragmenter::maxPayloadPerFragment();

	// Not a tight assertion, just a guard against the computation collapsing to something absurd if the
	// Video message ever grows a large mandatory field.
	QVERIFY(maxPayload > 800);
	QVERIFY(maxPayload < MAX_VIDEO_PROTOBUF_SIZE);

	QCOMPARE(VideoFragmenter::maxUnitSize(), MAX_VIDEO_FRAGMENTS_PER_UNIT * maxPayload);
}

void TestVideoFragmentation::everyPacketFitsTheMTU() {
	VideoFragmenter fragmenter;

	// The largest unit that is allowed at all, which is where the budget is tightest.
	const std::vector< byte > payload = makePayload(VideoFragmenter::maxUnitSize());

	QVERIFY(fragmenter.fragment(makeHeader(), payload));
	QCOMPARE(fragmenter.packets().size(), static_cast< std::size_t >(MAX_VIDEO_FRAGMENTS_PER_UNIT));

	for (const std::vector< byte > &packet : fragmenter.packets()) {
		QVERIFY(packet.size() + WIRE_OVERHEAD <= MAX_VIDEO_DATAGRAM_SIZE);
	}
}

void TestVideoFragmentation::worstCaseHeaderFitsTheMTU() {
	VideoFragmenter fragmenter;

	QVERIFY(fragmenter.fragment(makeMaxHeader(), makePayload(VideoFragmenter::maxUnitSize())));
	QCOMPARE(fragmenter.packets().size(), static_cast< std::size_t >(MAX_VIDEO_FRAGMENTS_PER_UNIT));

	for (const std::vector< byte > &packet : fragmenter.packets()) {
		QVERIFY(packet.size() + WIRE_OVERHEAD <= MAX_VIDEO_DATAGRAM_SIZE);
	}
}

void TestVideoFragmentation::worstCaseSurvivesRelayStamping() {
	VideoFragmenter fragmenter;

	// A sender that already encoded the widest possible sender_session, whose packets a server then
	// stamps again by appending. Both copies have to fit, and the appended one has to win.
	QVERIFY(fragmenter.fragment(makeMaxHeader(), makePayload(VideoFragmenter::maxUnitSize())));

	constexpr std::uint32_t RELAY_SESSION = 0xABCDEF12;

	for (const std::vector< byte > &packet : fragmenter.packets()) {
		const std::vector< byte > stamped = appendSenderSessionStamp(packet, RELAY_SESSION);

		QVERIFY(stamped.size() + WIRE_OVERHEAD <= MAX_VIDEO_DATAGRAM_SIZE);

		// Protobuf's last-wins rule for singular scalars is what makes the append trick legitimate.
		MumbleUDP::Video message;
		QVERIFY(message.ParseFromArray(stamped.data() + 1, static_cast< int >(stamped.size() - 1)));
		QCOMPARE(message.sender_session(), RELAY_SESSION);
	}
}

void TestVideoFragmentation::roundtripEmptyPayload() {
	VideoFragmenter fragmenter;
	VideoReassembler reassembler;

	const VideoUnitHeader header = makeHeader();

	QVERIFY(fragmenter.fragment(header, {}));
	QCOMPARE(fragmenter.packets().size(), static_cast< std::size_t >(1));

	VideoUnit unit;
	QCOMPARE(reassembler.processPacket(fragmenter.packets()[0], SESSION, 0, unit), VideoReassemblyResult::Complete);
	QCOMPARE(unit.header, header);
	QVERIFY(unit.payload.empty());
}

void TestVideoFragmentation::roundtripSingleFragment() {
	VideoFragmenter fragmenter;
	VideoReassembler reassembler;

	const VideoUnitHeader header    = makeHeader();
	const std::vector< byte > input = makePayload(100);

	QVERIFY(fragmenter.fragment(header, input));
	QCOMPARE(fragmenter.packets().size(), static_cast< std::size_t >(1));

	VideoUnit unit;
	QCOMPARE(reassembler.processPacket(fragmenter.packets()[0], SESSION, 0, unit), VideoReassemblyResult::Complete);
	QCOMPARE(unit.header, header);
	QCOMPARE(unit.payload, input);
	QCOMPARE(reassembler.pendingUnitCount(), static_cast< std::size_t >(0));
}

void TestVideoFragmentation::roundtripMultiFragment() {
	VideoFragmenter fragmenter;
	VideoReassembler reassembler;

	const VideoUnitHeader header    = makeHeader();
	const std::vector< byte > input = makePayload(VideoFragmenter::maxPayloadPerFragment() * 5 + 17);

	QVERIFY(fragmenter.fragment(header, input));
	QCOMPARE(fragmenter.packets().size(), static_cast< std::size_t >(6));

	VideoUnit unit;

	for (std::size_t i = 0; i < fragmenter.packets().size(); ++i) {
		const VideoReassemblyResult result = reassembler.processPacket(fragmenter.packets()[i], SESSION, 0, unit);

		if (i + 1 < fragmenter.packets().size()) {
			QCOMPARE(result, VideoReassemblyResult::Incomplete);
		} else {
			QCOMPARE(result, VideoReassemblyResult::Complete);
		}
	}

	QCOMPARE(unit.header, header);
	QCOMPARE(unit.payload, input);
	QCOMPARE(reassembler.pendingUnitCount(), static_cast< std::size_t >(0));
}

void TestVideoFragmentation::roundtripAtMaxFragmentCount() {
	VideoFragmenter fragmenter;
	VideoReassembler reassembler;

	// The largest unit the design permits, driven all the way through reassembly rather than only
	// checked for packet size.
	const VideoUnitHeader header    = makeMaxHeader();
	const std::vector< byte > input = makePayload(VideoFragmenter::maxUnitSize());

	QVERIFY(fragmenter.fragment(header, input));
	QCOMPARE(fragmenter.packets().size(), static_cast< std::size_t >(MAX_VIDEO_FRAGMENTS_PER_UNIT));

	VideoUnit unit;
	VideoReassemblyResult result = VideoReassemblyResult::Invalid;

	for (const std::vector< byte > &packet : fragmenter.packets()) {
		result = reassembler.processPacket(packet, header.senderSession, 0, unit);
	}

	QCOMPARE(result, VideoReassemblyResult::Complete);
	QCOMPARE(unit.header, header);
	QCOMPARE(unit.payload, input);
}

void TestVideoFragmentation::fragmentCountsAtBoundaries() {
	const std::size_t maxPayload = VideoFragmenter::maxPayloadPerFragment();

	VideoFragmenter fragmenter;

	QVERIFY(fragmenter.fragment(makeHeader(), makePayload(maxPayload)));
	QCOMPARE(fragmenter.packets().size(), static_cast< std::size_t >(1));

	QVERIFY(fragmenter.fragment(makeHeader(), makePayload(maxPayload + 1)));
	QCOMPARE(fragmenter.packets().size(), static_cast< std::size_t >(2));

	QVERIFY(fragmenter.fragment(makeHeader(), makePayload(maxPayload * 2)));
	QCOMPARE(fragmenter.packets().size(), static_cast< std::size_t >(2));
}

void TestVideoFragmentation::outOfOrderDeliveryReassembles() {
	VideoFragmenter fragmenter;
	VideoReassembler reassembler;

	const VideoUnitHeader header    = makeHeader();
	const std::vector< byte > input = makePayload(VideoFragmenter::maxPayloadPerFragment() * 3);

	QVERIFY(fragmenter.fragment(header, input));
	QCOMPARE(fragmenter.packets().size(), static_cast< std::size_t >(3));

	VideoUnit unit;

	// Deliver last first, so that fragment 0 -- the only one carrying geometry -- arrives at the end.
	QCOMPARE(reassembler.processPacket(fragmenter.packets()[2], SESSION, 0, unit), VideoReassemblyResult::Incomplete);
	QCOMPARE(reassembler.processPacket(fragmenter.packets()[1], SESSION, 0, unit), VideoReassemblyResult::Incomplete);
	QCOMPARE(reassembler.processPacket(fragmenter.packets()[0], SESSION, 0, unit), VideoReassemblyResult::Complete);

	QCOMPARE(unit.header, header);
	QCOMPARE(unit.payload, input);
}

void TestVideoFragmentation::duplicateFragmentsAreIgnored() {
	VideoFragmenter fragmenter;
	VideoReassembler reassembler;

	const VideoUnitHeader header    = makeHeader();
	const std::vector< byte > input = makePayload(VideoFragmenter::maxPayloadPerFragment() * 2);

	QVERIFY(fragmenter.fragment(header, input));

	VideoUnit unit;

	QCOMPARE(reassembler.processPacket(fragmenter.packets()[0], SESSION, 0, unit), VideoReassemblyResult::Incomplete);
	// The same fragment several times must not be mistaken for progress.
	QCOMPARE(reassembler.processPacket(fragmenter.packets()[0], SESSION, 0, unit), VideoReassemblyResult::Incomplete);
	QCOMPARE(reassembler.processPacket(fragmenter.packets()[0], SESSION, 0, unit), VideoReassemblyResult::Incomplete);
	QCOMPARE(reassembler.processPacket(fragmenter.packets()[1], SESSION, 0, unit), VideoReassemblyResult::Complete);

	QCOMPARE(unit.payload, input);
}

void TestVideoFragmentation::duplicateWithDifferentBytesKeepsTheFirst() {
	VideoReassembler reassembler;
	VideoUnit unit;

	MumbleUDP::Video first;
	first.set_fragment_count(2);
	first.set_fragment_index(0);
	first.set_payload("AAA");

	MumbleUDP::Video impostor;
	impostor.set_fragment_count(2);
	impostor.set_fragment_index(0);
	impostor.set_payload("BBB");

	MumbleUDP::Video second;
	second.set_fragment_count(2);
	second.set_fragment_index(1);
	second.set_payload("ZZZ");

	QCOMPARE(reassembler.processPacket(makePacket(first), SESSION, 0, unit), VideoReassemblyResult::Incomplete);
	QCOMPARE(reassembler.processPacket(makePacket(impostor), SESSION, 0, unit), VideoReassemblyResult::Incomplete);
	QCOMPARE(reassembler.processPacket(makePacket(second), SESSION, 0, unit), VideoReassemblyResult::Complete);

	// The first copy wins, so a late rewrite of an already-received fragment cannot alter the result.
	const std::vector< byte > expected = { 'A', 'A', 'A', 'Z', 'Z', 'Z' };
	QCOMPARE(unit.payload, expected);
}

void TestVideoFragmentation::oversizedUnitIsRejected() {
	VideoFragmenter fragmenter;

	QVERIFY(!fragmenter.fragment(makeHeader(), makePayload(VideoFragmenter::maxUnitSize() + 1)));
	QVERIFY(fragmenter.packets().empty());
}

void TestVideoFragmentation::rejectsWrongMessageType() {
	VideoFragmenter fragmenter;
	VideoReassembler reassembler;

	QVERIFY(fragmenter.fragment(makeHeader(), makePayload(50)));

	std::vector< byte > packet = fragmenter.packets()[0];
	packet[0]                  = static_cast< byte >(UDPMessageType::Audio);

	VideoUnit unit;
	QCOMPARE(reassembler.processPacket(packet, SESSION, 0, unit), VideoReassemblyResult::Invalid);
}

void TestVideoFragmentation::rejectsTruncatedPacket() {
	VideoReassembler reassembler;
	VideoUnit unit;

	const std::vector< byte > empty;
	QCOMPARE(reassembler.processPacket(empty, SESSION, 0, unit), VideoReassemblyResult::Invalid);

	const std::vector< byte > typeOnly = { static_cast< byte >(UDPMessageType::Video) };
	QCOMPARE(reassembler.processPacket(typeOnly, SESSION, 0, unit), VideoReassemblyResult::Invalid);
}

void TestVideoFragmentation::rejectsMalformedProtobuf() {
	VideoReassembler reassembler;
	VideoUnit unit;

	// A well-formed message-type prefix followed by bytes that are not a valid Video message: a varint
	// that never terminates, then a tag with an unknown wire type.
	std::vector< byte > packet = { static_cast< byte >(UDPMessageType::Video) };
	packet.insert(packet.end(), 12, 0xFF);

	QCOMPARE(reassembler.processPacket(packet, SESSION, 0, unit), VideoReassemblyResult::Invalid);
	QCOMPARE(reassembler.pendingUnitCount(), static_cast< std::size_t >(0));

	// A packet claiming more protobuf than the budget permits is refused before parsing.
	std::vector< byte > oversized(MAX_VIDEO_PROTOBUF_SIZE + 2, 0x00);
	oversized[0] = static_cast< byte >(UDPMessageType::Video);

	QCOMPARE(reassembler.processPacket(oversized, SESSION, 0, unit), VideoReassemblyResult::Invalid);
}

void TestVideoFragmentation::rejectsZeroFragmentCount() {
	MumbleUDP::Video message;
	message.set_frame_number(5);
	message.set_fragment_index(0);
	// fragment_count deliberately left at its default of zero.

	VideoReassembler reassembler;
	VideoUnit unit;

	QCOMPARE(reassembler.processPacket(makePacket(message), SESSION, 0, unit), VideoReassemblyResult::Invalid);
	QCOMPARE(reassembler.pendingUnitCount(), static_cast< std::size_t >(0));
}

void TestVideoFragmentation::rejectsExcessiveFragmentCount() {
	// The interesting hostile case: a tiny packet claiming an enormous unit, which must not cause a
	// correspondingly enormous allocation.
	MumbleUDP::Video message;
	message.set_fragment_index(0);
	message.set_fragment_count(std::numeric_limits< std::uint32_t >::max());

	VideoReassembler reassembler;
	VideoUnit unit;

	QCOMPARE(reassembler.processPacket(makePacket(message), SESSION, 0, unit), VideoReassemblyResult::Invalid);
	QCOMPARE(reassembler.pendingUnitCount(), static_cast< std::size_t >(0));

	// One past the limit must be refused just as firmly as the absurd value.
	message.set_fragment_count(MAX_VIDEO_FRAGMENTS_PER_UNIT + 1);
	QCOMPARE(reassembler.processPacket(makePacket(message), SESSION, 0, unit), VideoReassemblyResult::Invalid);
	QCOMPARE(reassembler.pendingUnitCount(), static_cast< std::size_t >(0));
}

void TestVideoFragmentation::rejectsFragmentIndexOutOfRange() {
	MumbleUDP::Video message;
	message.set_fragment_count(4);
	message.set_fragment_index(4);

	VideoReassembler reassembler;
	VideoUnit unit;

	QCOMPARE(reassembler.processPacket(makePacket(message), SESSION, 0, unit), VideoReassemblyResult::Invalid);
	QCOMPARE(reassembler.pendingUnitCount(), static_cast< std::size_t >(0));
}

void TestVideoFragmentation::rejectsInconsistentFragmentCount() {
	VideoReassembler reassembler;
	VideoUnit unit;

	MumbleUDP::Video first;
	first.set_stream_id(1);
	first.set_fragment_count(4);
	first.set_fragment_index(0);
	first.set_payload("abc");

	QCOMPARE(reassembler.processPacket(makePacket(first), SESSION, 0, unit), VideoReassemblyResult::Incomplete);

	// Same unit, different claim about its size.
	MumbleUDP::Video second;
	second.set_stream_id(1);
	second.set_fragment_count(3);
	second.set_fragment_index(1);
	second.set_payload("def");

	QCOMPARE(reassembler.processPacket(makePacket(second), SESSION, 0, unit), VideoReassemblyResult::Invalid);

	// The partial unit must have been dropped, not left in a half-trusted state.
	QCOMPARE(reassembler.pendingUnitCount(), static_cast< std::size_t >(0));
}

void TestVideoFragmentation::rejectsOversizedReassembledUnit() {
	VideoReassembler reassembler;
	VideoUnit unit;

	// Each fragment is individually legal, but their payloads sum past what a unit may hold. The
	// fragmenter would never produce this; a hostile peer trivially can, because the per-fragment budget
	// reserves room for a worst-case header and a hand-built packet need not use it.
	//
	// Derived from the budget rather than hardcoded, so that this keeps testing the limit rather than
	// silently becoming a no-op the next time the MTU or the header changes.
	const std::string chunk(VideoFragmenter::maxPayloadPerFragment() + 32, 'x');

	VideoReassemblyResult result = VideoReassemblyResult::Incomplete;

	for (std::uint32_t i = 0; i < MAX_VIDEO_FRAGMENTS_PER_UNIT; ++i) {
		MumbleUDP::Video message;
		message.set_fragment_count(MAX_VIDEO_FRAGMENTS_PER_UNIT);
		message.set_fragment_index(i);
		message.set_payload(chunk);

		result = reassembler.processPacket(makePacket(message), SESSION, 0, unit);

		if (result == VideoReassemblyResult::Invalid) {
			break;
		}
	}

	QCOMPARE(result, VideoReassemblyResult::Invalid);
	QCOMPARE(reassembler.pendingUnitCount(), static_cast< std::size_t >(0));
}

void TestVideoFragmentation::rejectsSpoofedSenderSession() {
	MumbleUDP::Video message;
	message.set_sender_session(SESSION + 1);
	message.set_fragment_count(2);
	message.set_fragment_index(0);
	message.set_payload("x");

	VideoReassembler reassembler;
	VideoUnit unit;

	// The packet claims to be someone else than the connection it arrived on.
	QCOMPARE(reassembler.processPacket(makePacket(message), SESSION, 0, unit), VideoReassemblyResult::Invalid);
	QCOMPARE(reassembler.pendingUnitCount(), static_cast< std::size_t >(0));
}

void TestVideoFragmentation::spoofingCannotEvadeThePerSenderLimit() {
	VideoReassembler reassembler;
	VideoUnit unit;

	// Minting a fresh identity per packet is the cheapest way to make a per-sender limit meaningless, so
	// it must not work: every one of these is refused outright.
	for (std::uint32_t claimed = 1; claimed <= 100; ++claimed) {
		if (claimed == SESSION) {
			// Naming its own session truthfully is not spoofing, and is accepted.
			continue;
		}

		MumbleUDP::Video message;
		message.set_sender_session(claimed);
		message.set_frame_number(claimed);
		message.set_fragment_count(MAX_VIDEO_FRAGMENTS_PER_UNIT);
		message.set_fragment_index(0);
		message.set_payload("x");

		QCOMPARE(reassembler.processPacket(makePacket(message), SESSION, 0, unit), VideoReassemblyResult::Invalid);
	}

	QCOMPARE(reassembler.pendingUnitCount(), static_cast< std::size_t >(0));

	// Leaving the field unset is legitimate, but then the per-sender cap applies with full force.
	for (std::uint32_t frame = 0; frame < MAX_PENDING_VIDEO_UNITS_PER_SENDER * 2; ++frame) {
		MumbleUDP::Video message;
		message.set_frame_number(frame);
		message.set_fragment_count(MAX_VIDEO_FRAGMENTS_PER_UNIT);
		message.set_fragment_index(0);
		message.set_payload("x");

		QCOMPARE(reassembler.processPacket(makePacket(message), SESSION, frame, unit),
				 VideoReassemblyResult::Incomplete);
	}

	QCOMPARE(reassembler.pendingUnitCount(), MAX_PENDING_VIDEO_UNITS_PER_SENDER);
}

void TestVideoFragmentation::spoofingCannotCorruptAnotherSendersUnit() {
	VideoReassembler reassembler;
	VideoUnit unit;

	// The victim opens a two-fragment unit.
	MumbleUDP::Video victimFirst;
	victimFirst.set_fragment_count(2);
	victimFirst.set_fragment_index(0);
	victimFirst.set_payload("GOOD");

	QCOMPARE(reassembler.processPacket(makePacket(victimFirst), 1, 0, unit), VideoReassemblyResult::Incomplete);

	// An attacker on a different connection sends a fragment addressed at the victim's unit, both by
	// leaving sender_session unset and by naming it explicitly.
	MumbleUDP::Video attack;
	attack.set_fragment_count(2);
	attack.set_fragment_index(1);
	attack.set_payload("EVIL");

	QCOMPARE(reassembler.processPacket(makePacket(attack), 2, 0, unit), VideoReassemblyResult::Incomplete);

	attack.set_sender_session(1);
	QCOMPARE(reassembler.processPacket(makePacket(attack), 2, 0, unit), VideoReassemblyResult::Invalid);

	// The victim's unit completes with only the victim's own bytes.
	MumbleUDP::Video victimSecond;
	victimSecond.set_fragment_count(2);
	victimSecond.set_fragment_index(1);
	victimSecond.set_payload("ALSOGOOD");

	QCOMPARE(reassembler.processPacket(makePacket(victimSecond), 1, 0, unit), VideoReassemblyResult::Complete);

	const std::vector< byte > expected = { 'G', 'O', 'O', 'D', 'A', 'L', 'S', 'O', 'G', 'O', 'O', 'D' };
	QCOMPARE(unit.payload, expected);
	QCOMPARE(unit.header.senderSession, static_cast< std::uint32_t >(1));
}

void TestVideoFragmentation::distinctSendersReassembleIndependently() {
	VideoReassembler reassembler;
	VideoUnit unit;

	// Identical stream, frame and unit IDs from two different senders must not collide.
	for (std::uint32_t session : { 1u, 2u }) {
		MumbleUDP::Video message;
		message.set_fragment_count(2);
		message.set_fragment_index(0);
		message.set_payload("x");

		QCOMPARE(reassembler.processPacket(makePacket(message), session, 0, unit), VideoReassemblyResult::Incomplete);
	}

	QCOMPARE(reassembler.pendingUnitCount(), static_cast< std::size_t >(2));
}

void TestVideoFragmentation::stalePartialUnitsExpire() {
	VideoFragmenter fragmenter;
	VideoReassembler reassembler;

	QVERIFY(fragmenter.fragment(makeHeader(), makePayload(VideoFragmenter::maxPayloadPerFragment() * 2)));

	VideoUnit unit;
	QCOMPARE(reassembler.processPacket(fragmenter.packets()[0], SESSION, 1000, unit),
			 VideoReassemblyResult::Incomplete);
	QCOMPARE(reassembler.pendingUnitCount(), static_cast< std::size_t >(1));

	// Exactly at the deadline the unit is still held; one microsecond later it is not.
	reassembler.expire(1000 + VIDEO_REASSEMBLY_TIMEOUT_USEC);
	QCOMPARE(reassembler.pendingUnitCount(), static_cast< std::size_t >(1));

	reassembler.expire(1000 + VIDEO_REASSEMBLY_TIMEOUT_USEC + 1);
	QCOMPARE(reassembler.pendingUnitCount(), static_cast< std::size_t >(0));

	// The other half arriving after the timeout starts a fresh partial unit rather than completing the
	// discarded one.
	QCOMPARE(
		reassembler.processPacket(fragmenter.packets()[1], SESSION, 1000 + VIDEO_REASSEMBLY_TIMEOUT_USEC + 2, unit),
		VideoReassemblyResult::Incomplete);
	QCOMPARE(reassembler.pendingUnitCount(), static_cast< std::size_t >(1));
}

void TestVideoFragmentation::processPacketDrivesExpiry() {
	VideoReassembler reassembler;
	VideoUnit unit;

	MumbleUDP::Video first;
	first.set_stream_id(1);
	first.set_fragment_count(2);
	first.set_fragment_index(0);
	first.set_payload("x");

	QCOMPARE(reassembler.processPacket(makePacket(first), SESSION, 1000, unit), VideoReassemblyResult::Incomplete);
	QCOMPARE(reassembler.pendingUnitCount(), static_cast< std::size_t >(1));

	// An unrelated packet arriving late is enough to sweep the stale unit; no separate expire() call.
	MumbleUDP::Video later;
	later.set_stream_id(2);
	later.set_fragment_count(2);
	later.set_fragment_index(0);
	later.set_payload("y");

	QCOMPARE(reassembler.processPacket(makePacket(later), SESSION, 1000 + VIDEO_REASSEMBLY_TIMEOUT_USEC + 1, unit),
			 VideoReassemblyResult::Incomplete);

	// Only the newly created one survives.
	QCOMPARE(reassembler.pendingUnitCount(), static_cast< std::size_t >(1));
}

void TestVideoFragmentation::pendingUnitsAreCappedPerSender() {
	VideoReassembler reassembler;
	VideoUnit unit;

	// Open far more partial units than the limit allows, each one distinct.
	for (std::uint32_t i = 0; i < MAX_PENDING_VIDEO_UNITS_PER_SENDER * 4; ++i) {
		MumbleUDP::Video message;
		message.set_frame_number(i);
		message.set_fragment_count(2);
		message.set_fragment_index(0);
		message.set_payload("x");

		QCOMPARE(reassembler.processPacket(makePacket(message), SESSION, i, unit), VideoReassemblyResult::Incomplete);
	}

	QCOMPARE(reassembler.pendingUnitCount(), MAX_PENDING_VIDEO_UNITS_PER_SENDER);
}

void TestVideoFragmentation::pendingUnitsAreCappedGlobally() {
	VideoReassembler reassembler;
	VideoUnit unit;

	// Many distinct legitimate senders, each within its own per-sender allowance. The per-sender cap
	// alone would let total memory grow with the number of sessions, so the global caps have to bite.
	//
	// The units here are deliberately fat -- three near-full fragments each -- because that is what makes
	// the byte budget, rather than the unit count, the binding constraint. With one-byte payloads the
	// count ceiling would be hit first and the memory bound would never be exercised at all.
	const std::string chunk(VideoFragmenter::maxPayloadPerFragment(), 'x');

	std::uint64_t clock = 1;

	for (std::uint32_t session = 1; session <= 400; ++session) {
		for (std::uint32_t frame = 0; frame < 40; ++frame) {
			for (std::uint32_t fragment = 0; fragment < 3; ++fragment) {
				MumbleUDP::Video message;
				message.set_frame_number(frame);
				message.set_fragment_count(MAX_VIDEO_FRAGMENTS_PER_UNIT);
				message.set_fragment_index(fragment);
				message.set_payload(chunk);

				QCOMPARE(reassembler.processPacket(makePacket(message), session, clock++, unit),
						 VideoReassemblyResult::Incomplete);
			}
		}
	}

	QVERIFY(reassembler.pendingUnitCount() <= MAX_PENDING_VIDEO_UNITS_TOTAL);
	QVERIFY(reassembler.bufferedBytes() <= MAX_PENDING_VIDEO_BYTES_TOTAL);

	// The byte budget is what should have bound here, not the unit count.
	QVERIFY(reassembler.bufferedBytes() > MAX_PENDING_VIDEO_BYTES_TOTAL / 2);

	// And the accounting must be exact, not merely bounded: draining every sender has to return the
	// buffered total to zero, or the bound leaks a little on every disconnect.
	for (std::uint32_t session = 1; session <= 400; ++session) {
		reassembler.removeSender(session);
	}

	QCOMPARE(reassembler.pendingUnitCount(), static_cast< std::size_t >(0));
	QCOMPARE(reassembler.bufferedBytes(), static_cast< std::size_t >(0));
}

void TestVideoFragmentation::removeSenderDropsOnlyThatSender() {
	VideoReassembler reassembler;
	VideoUnit unit;

	// Two streams for the first sender, so that removal has to clear all of them, and boundary session
	// IDs to exercise the range lookup.
	for (std::uint32_t stream = 0; stream < 2; ++stream) {
		MumbleUDP::Video message;
		message.set_stream_id(stream);
		message.set_fragment_count(2);
		message.set_fragment_index(0);
		message.set_payload("x");

		QCOMPARE(reassembler.processPacket(makePacket(message), 0, 0, unit), VideoReassemblyResult::Incomplete);
	}

	MumbleUDP::Video other;
	other.set_fragment_count(2);
	other.set_fragment_index(0);
	other.set_payload("x");

	QCOMPARE(reassembler.processPacket(makePacket(other), std::numeric_limits< std::uint32_t >::max(), 0, unit),
			 VideoReassemblyResult::Incomplete);

	QCOMPARE(reassembler.pendingUnitCount(), static_cast< std::size_t >(3));

	reassembler.removeSender(0);

	QCOMPARE(reassembler.pendingUnitCount(), static_cast< std::size_t >(1));

	reassembler.removeSender(std::numeric_limits< std::uint32_t >::max());

	QCOMPARE(reassembler.pendingUnitCount(), static_cast< std::size_t >(0));
}

QTEST_MAIN(TestVideoFragmentation)
#include "TestVideoFragmentation.moc"
