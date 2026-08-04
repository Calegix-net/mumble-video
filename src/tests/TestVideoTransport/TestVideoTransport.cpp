// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "VideoFragmentation.h"
#include "VideoTransport.h"
#include "crypto/CryptStateOCB2.h"

#include <QObject>
#include <QtTest>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using namespace Mumble::Protocol;

namespace {

const std::string KEY(AES_KEY_SIZE_BYTES, '\x11');
const std::string BASE_A(12, '\x22');
const std::string BASE_B(12, '\x33');

/// A keyed pair, sender to receiver.
struct Pair {
	VideoCryptState sender;
	VideoCryptState receiver;

	Pair() {
		sender.setKey(KEY, BASE_A, BASE_B);
		receiver.setKey(KEY, BASE_B, BASE_A);
	}
};

std::vector< byte > makePlaintext(std::size_t size, byte fill) {
	return std::vector< byte >(size, fill);
}

} // namespace

class TestVideoTransport : public QObject {
	Q_OBJECT
private slots:
	void keyingRequiresDistinctNonceBases();
	void roundTripRecoversThePlaintext();
	void sequenceNumbersAdvance();
	void aFullSizedFragmentFitsTheDatagram();
	void reorderedBurstOfEveryFragmentSurvives();
	void replayedPacketIsRejected();
	void packetOlderThanTheWindowIsRejected();
	void tamperedCiphertextIsRejected();
	void tamperedSequenceIsRejected();
	void wrongChannelIsRejected();
	void truncatedDatagramIsRejected();
	void forgeryDoesNotSuppressTheGenuinePacket();
	void videoTrafficDoesNotDisturbAudio();
	void derivedKeysAgreeAcrossTheConnection();
	void derivedKeysDifferFromTheSessionKey();
	void theReceiveBufferFitsBothChannels();
};

void TestVideoTransport::keyingRequiresDistinctNonceBases() {
	VideoCryptState state;

	// Same base both directions would mean the same nonce for the same sequence under one key, which for
	// OCB is a total break rather than a weakness. It must be refused outright.
	QVERIFY(!state.setKey(KEY, BASE_A, BASE_A));
	QVERIFY(!state.isValid());

	QVERIFY(state.setKey(KEY, BASE_A, BASE_B));
	QVERIFY(state.isValid());

	// Wrong sizes are refused too.
	QVERIFY(!state.setKey(std::string(8, 'x'), BASE_A, BASE_B));
	QVERIFY(!state.setKey(KEY, std::string(4, 'x'), BASE_B));
}

void TestVideoTransport::roundTripRecoversThePlaintext() {
	Pair pair;

	const std::vector< byte > plaintext = makePlaintext(700, 0xAB);

	std::vector< byte > datagram;
	QVERIFY(pair.sender.encrypt(plaintext, datagram));

	QCOMPARE(datagram[0], MEDIA_CHANNEL_VIDEO);
	QCOMPARE(datagram.size(), plaintext.size() + VIDEO_HEADER_SIZE);

	std::vector< byte > recovered;
	QCOMPARE(pair.receiver.decrypt(datagram, recovered), VideoCryptState::Result::Ok);
	QCOMPARE(recovered, plaintext);
}

void TestVideoTransport::sequenceNumbersAdvance() {
	Pair pair;

	QCOMPARE(pair.sender.nextSequence(), VIDEO_FIRST_SEQUENCE);
	QCOMPARE(pair.receiver.highestReceived(), 0u);

	std::vector< byte > datagram;
	std::vector< byte > recovered;

	for (std::uint32_t i = 0; i < 10; ++i) {
		QVERIFY(pair.sender.encrypt(makePlaintext(64, static_cast< byte >(i)), datagram));
		QCOMPARE(pair.receiver.decrypt(datagram, recovered), VideoCryptState::Result::Ok);
		QCOMPARE(pair.receiver.highestReceived(), VIDEO_FIRST_SEQUENCE + i);
	}

	QCOMPARE(pair.sender.nextSequence(), VIDEO_FIRST_SEQUENCE + 10);
}

void TestVideoTransport::aFullSizedFragmentFitsTheDatagram() {
	Pair pair;

	// The largest thing the fragmenter will ever hand the transport: a full protobuf plus its
	// message-type byte. It must fit in one datagram with room for the transport header.
	const std::vector< byte > plaintext = makePlaintext(MAX_VIDEO_PROTOBUF_SIZE + 1, 0x5A);

	std::vector< byte > datagram;
	QVERIFY(pair.sender.encrypt(plaintext, datagram));
	QVERIFY(datagram.size() <= MAX_VIDEO_DATAGRAM_SIZE);

	std::vector< byte > recovered;
	QCOMPARE(pair.receiver.decrypt(datagram, recovered), VideoCryptState::Result::Ok);
	QCOMPARE(recovered, plaintext);

	// One byte more than the transport allows must be refused rather than truncated.
	QVERIFY(!pair.sender.encrypt(makePlaintext(MAX_VIDEO_PLAINTEXT_SIZE + 1, 0x5A), datagram));
}

void TestVideoTransport::reorderedBurstOfEveryFragmentSurvives() {
	// This is the test the old transport could not pass. Sharing audio's crypt state meant a fixed
	// 30-packet tolerance: a burst of 31 delivered backwards already lost packets, and a full 64-fragment
	// unit lost over half. With video on its own sequence and a 128-slot window, a whole maximum-size
	// unit arriving in reverse must decrypt completely.
	Pair pair;

	std::vector< std::vector< byte > > burst;

	for (std::uint32_t i = 0; i < MAX_VIDEO_FRAGMENTS_PER_UNIT; ++i) {
		std::vector< byte > datagram;
		QVERIFY(pair.sender.encrypt(makePlaintext(900, static_cast< byte >(i)), datagram));
		burst.push_back(std::move(datagram));
	}

	std::reverse(burst.begin(), burst.end());

	unsigned int recoveredCount = 0;
	std::vector< byte > recovered;

	for (const std::vector< byte > &datagram : burst) {
		if (pair.receiver.decrypt(datagram, recovered) == VideoCryptState::Result::Ok) {
			recoveredCount++;
		}
	}

	QCOMPARE(recoveredCount, MAX_VIDEO_FRAGMENTS_PER_UNIT);
	QCOMPARE(pair.receiver.tooOldCount(), static_cast< std::uint64_t >(0));
	QCOMPARE(pair.receiver.authFailureCount(), static_cast< std::uint64_t >(0));
}

void TestVideoTransport::replayedPacketIsRejected() {
	Pair pair;

	std::vector< byte > datagram;
	QVERIFY(pair.sender.encrypt(makePlaintext(100, 0x01), datagram));

	std::vector< byte > recovered;
	QCOMPARE(pair.receiver.decrypt(datagram, recovered), VideoCryptState::Result::Ok);

	// The identical datagram again is a replay, whether from a hostile peer or a duplicating network.
	QCOMPARE(pair.receiver.decrypt(datagram, recovered), VideoCryptState::Result::Replay);
	QCOMPARE(pair.receiver.replayCount(), static_cast< std::uint64_t >(1));
}

void TestVideoTransport::packetOlderThanTheWindowIsRejected() {
	Pair pair;

	std::vector< byte > first;
	QVERIFY(pair.sender.encrypt(makePlaintext(100, 0x01), first));

	// Move the receiver well past the window without ever delivering the first packet.
	std::vector< byte > datagram;
	std::vector< byte > recovered;

	for (std::size_t i = 0; i < VIDEO_REPLAY_WINDOW + 10; ++i) {
		QVERIFY(pair.sender.encrypt(makePlaintext(100, 0x02), datagram));
		QCOMPARE(pair.receiver.decrypt(datagram, recovered), VideoCryptState::Result::Ok);
	}

	// Now the straggler is too far behind to be distinguishable from a replay.
	QCOMPARE(pair.receiver.decrypt(first, recovered), VideoCryptState::Result::TooOld);
	QCOMPARE(pair.receiver.tooOldCount(), static_cast< std::uint64_t >(1));
}

void TestVideoTransport::tamperedCiphertextIsRejected() {
	Pair pair;

	std::vector< byte > datagram;
	QVERIFY(pair.sender.encrypt(makePlaintext(200, 0x7C), datagram));

	datagram[VIDEO_HEADER_SIZE + 20] ^= 0xFF;

	std::vector< byte > recovered;
	QCOMPARE(pair.receiver.decrypt(datagram, recovered), VideoCryptState::Result::AuthenticationFailed);
	QVERIFY(recovered.empty());
}

void TestVideoTransport::tamperedSequenceIsRejected() {
	Pair pair;

	std::vector< byte > datagram;
	QVERIFY(pair.sender.encrypt(makePlaintext(200, 0x7C), datagram));

	// The sequence is in the clear, so an attacker can change it - but it is also the nonce, so changing
	// it makes the tag fail to verify. That is what stops it being a free re-ordering primitive.
	datagram[VIDEO_SEQUENCE_OFFSET + 3] ^= 0x40;

	std::vector< byte > recovered;
	QCOMPARE(pair.receiver.decrypt(datagram, recovered), VideoCryptState::Result::AuthenticationFailed);
}

void TestVideoTransport::wrongChannelIsRejected() {
	Pair pair;

	std::vector< byte > datagram;
	QVERIFY(pair.sender.encrypt(makePlaintext(100, 0x01), datagram));

	datagram[0] = MEDIA_CHANNEL_AUDIO;

	std::vector< byte > recovered;
	QCOMPARE(pair.receiver.decrypt(datagram, recovered), VideoCryptState::Result::WrongChannel);
}

void TestVideoTransport::truncatedDatagramIsRejected() {
	Pair pair;

	std::vector< byte > recovered;

	for (std::size_t size = 0; size <= VIDEO_HEADER_SIZE; ++size) {
		const std::vector< byte > runt(size, MEDIA_CHANNEL_VIDEO);
		QCOMPARE(pair.receiver.decrypt(runt, recovered), VideoCryptState::Result::Malformed);
	}
}

void TestVideoTransport::forgeryDoesNotSuppressTheGenuinePacket() {
	Pair pair;

	std::vector< byte > genuine;
	QVERIFY(pair.sender.encrypt(makePlaintext(150, 0x3D), genuine));

	// A forgery claiming the same sequence number, with garbage where the ciphertext should be. If the
	// replay window were advanced before authentication, this would mark the sequence as seen and the
	// real packet would be discarded as a replay when it arrived.
	std::vector< byte > forged = genuine;
	forged[VIDEO_HEADER_SIZE] ^= 0xFF;

	std::vector< byte > recovered;
	QCOMPARE(pair.receiver.decrypt(forged, recovered), VideoCryptState::Result::AuthenticationFailed);

	// The genuine packet still gets through.
	QCOMPARE(pair.receiver.decrypt(genuine, recovered), VideoCryptState::Result::Ok);
	QCOMPARE(recovered.size(), static_cast< std::size_t >(150));
}

void TestVideoTransport::videoTrafficDoesNotDisturbAudio() {
	// The reason the transport was separated at all. Audio keeps its own CryptStateOCB2 with its own
	// rolling IV; video runs alongside it on its own sequence. Previously both shared one counter, so a
	// burst of video advanced audio's sequence and shrank its jitter tolerance from roughly 600 ms to
	// under 100 ms. Here, an audio packet delayed far beyond video's entire burst must still decrypt.
	CryptStateOCB2 audioSender;
	CryptStateOCB2 audioReceiver;

	const std::string audioKey(AES_KEY_SIZE_BYTES, '\x44');
	const std::string ivA(AES_BLOCK_SIZE, '\x55');
	const std::string ivB(AES_BLOCK_SIZE, '\x66');

	QVERIFY(audioSender.setKey(audioKey, ivA, ivB));
	QVERIFY(audioReceiver.setKey(audioKey, ivB, ivA));

	Pair video;

	constexpr unsigned int AUDIO_LEN = 160;

	// Two audio packets, the second of which we hold back.
	std::vector< unsigned char > audioPlain(AUDIO_LEN, 0x09);
	std::vector< unsigned char > audioOne(AUDIO_LEN + 4);
	std::vector< unsigned char > audioTwo(AUDIO_LEN + 4);

	QVERIFY(audioSender.encrypt(audioPlain.data(), audioOne.data(), AUDIO_LEN));
	QVERIFY(audioSender.encrypt(audioPlain.data(), audioTwo.data(), AUDIO_LEN));

	std::vector< unsigned char > audioOut(AUDIO_LEN);
	QVERIFY(audioReceiver.decrypt(audioOne.data(), audioOut.data(), AUDIO_LEN + 4));

	// Now push far more video than audio's 30-packet tolerance would ever have survived.
	std::vector< byte > datagram;
	std::vector< byte > recovered;

	for (int i = 0; i < 500; ++i) {
		QVERIFY(video.sender.encrypt(makePlaintext(900, static_cast< byte >(i)), datagram));
		QCOMPARE(video.receiver.decrypt(datagram, recovered), VideoCryptState::Result::Ok);
	}

	// The held-back audio packet still decrypts. On a shared counter, 500 intervening video packets
	// would have put it hopelessly outside the window.
	QVERIFY(audioReceiver.decrypt(audioTwo.data(), audioOut.data(), AUDIO_LEN + 4));
}

void TestVideoTransport::derivedKeysAgreeAcrossTheConnection() {
	// Video needs no key exchange of its own: both ends derive from the session key CryptSetup already
	// negotiated, picking opposite nonce bases from their role.
	const std::string sessionKey(AES_KEY_SIZE_BYTES, '\x7E');

	VideoCryptState server;
	VideoCryptState client;

	QVERIFY(server.deriveFromSessionKey(sessionKey, true));
	QVERIFY(client.deriveFromSessionKey(sessionKey, false));

	std::vector< byte > datagram;
	std::vector< byte > recovered;

	const std::vector< byte > downstream = makePlaintext(300, 0x11);
	QVERIFY(server.encrypt(downstream, datagram));
	QCOMPARE(client.decrypt(datagram, recovered), VideoCryptState::Result::Ok);
	QCOMPARE(recovered, downstream);

	const std::vector< byte > upstream = makePlaintext(300, 0x22);
	QVERIFY(client.encrypt(upstream, datagram));
	QCOMPARE(server.decrypt(datagram, recovered), VideoCryptState::Result::Ok);
	QCOMPARE(recovered, upstream);

	// Both ends taking the same role would mean identical nonce bases, which setKey refuses.
	VideoCryptState alsoServer;
	QVERIFY(alsoServer.deriveFromSessionKey(sessionKey, true));

	QVERIFY(!server.deriveFromSessionKey(std::string(4, 'x'), true));
}

void TestVideoTransport::derivedKeysDifferFromTheSessionKey() {
	// The video key must not be the audio key. Audio's nonce is a rolling 16-byte IV and video's is a
	// base plus a counter, so under one key the two constructions could collide on a nonce, which for
	// OCB is a break rather than a weakness.
	const std::string sessionKey(AES_KEY_SIZE_BYTES, '\x5C');

	VideoCryptState derived;
	QVERIFY(derived.deriveFromSessionKey(sessionKey, true));

	// A state keyed directly with the session key must not be able to read what the derived one wrote.
	VideoCryptState imposter;
	QVERIFY(imposter.setKey(sessionKey, BASE_B, BASE_A));

	std::vector< byte > datagram;
	QVERIFY(derived.encrypt(makePlaintext(120, 0x33), datagram));

	std::vector< byte > recovered;
	QCOMPARE(imposter.decrypt(datagram, recovered), VideoCryptState::Result::AuthenticationFailed);

	// A different session key must produce a different video key.
	VideoCryptState other;
	QVERIFY(other.deriveFromSessionKey(std::string(AES_KEY_SIZE_BYTES, '\x5D'), false));
	QCOMPARE(other.decrypt(datagram, recovered), VideoCryptState::Result::AuthenticationFailed);
}

void TestVideoTransport::theReceiveBufferFitsBothChannels() {
	// Audio and video arrive on one socket and are told apart only after the datagram has been read, so
	// the receive buffer has to fit whichever is larger. Getting this wrong truncates video silently,
	// which on the wire looks exactly like packet loss.
	QVERIFY(MAX_MEDIA_DATAGRAM_SIZE >= MAX_VIDEO_DATAGRAM_SIZE);
	QVERIFY(MAX_MEDIA_DATAGRAM_SIZE >= MAX_AUDIO_DATAGRAM_SIZE);

	// Audio is the old payload plus its channel byte.
	QCOMPARE(MAX_AUDIO_DATAGRAM_SIZE, MAX_UDP_PACKET_SIZE + 1);

	// The two channel markers must differ, or the demultiplexer has nothing to go on.
	QVERIFY(MEDIA_CHANNEL_AUDIO != MEDIA_CHANNEL_VIDEO);

	// A maximum-size video datagram really is the binding case.
	Pair pair;
	std::vector< byte > datagram;
	QVERIFY(pair.sender.encrypt(makePlaintext(MAX_VIDEO_PLAINTEXT_SIZE, 0x1F), datagram));
	QCOMPARE(datagram.size(), MAX_VIDEO_DATAGRAM_SIZE);
	QVERIFY(datagram.size() <= MAX_MEDIA_DATAGRAM_SIZE);
}

QTEST_MAIN(TestVideoTransport)
#include "TestVideoTransport.moc"
