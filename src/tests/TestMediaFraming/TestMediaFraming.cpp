// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

// Loopback test for the media datagram framing.
//
// Audio and video share one UDP socket and are told apart by a channel byte in the clear. That framing
// is implemented inline in Server::run and ServerHandler::udpReady, which no unit test can reach, so
// this reproduces the exact contract both of them implement and drives it over a real socket with the
// real crypto and the real audio codec path.
//
// The reason it exists: adding the channel byte changed the audio wire format. If audio regressed, the
// fork would be worse than useless no matter how well video worked, and nothing else in the suite would
// have noticed.

#include "MumbleProtocol.h"
#include "VideoFragmentation.h"
#include "VideoTransport.h"
#include "crypto/CryptStateOCB2.h"

#include <QObject>
#include <QtNetwork/QUdpSocket>
#include <QtTest>

#include <cstdint>
#include <string>
#include <vector>

using namespace Mumble::Protocol;

namespace {

constexpr Version::full_t PROTOCOL = Version::fromComponents(1, 5, 0);
constexpr std::uint32_t SESSION    = 11;

/// Exactly what Server::sendMessage and ServerHandler::sendMessage do to an audio payload.
std::vector< byte > frameAudio(CryptStateOCB2 &crypt, std::span< const byte > plaintext) {
	std::vector< byte > datagram(plaintext.size() + 5);

	datagram[0] = MEDIA_CHANNEL_AUDIO;

	if (!crypt.encrypt(plaintext.data(), datagram.data() + 1, static_cast< unsigned int >(plaintext.size()))) {
		return {};
	}

	return datagram;
}

} // namespace

class TestMediaFraming : public QObject {
	Q_OBJECT
private slots:
	void initTestCase();
	void audioSurvivesTheChannelPrefix();
	void videoSurvivesTheSameSocket();
	void interleavedTrafficIsDemultiplexed();
	void aMaximumSizedVideoDatagramIsNotTruncated();

private:
	QUdpSocket m_sender;
	QUdpSocket m_receiver;
	quint16 m_port = 0;

	/// Sends a datagram and returns what came back off the wire.
	std::vector< byte > roundTripOverUdp(const std::vector< byte > &datagram);
};

void TestMediaFraming::initTestCase() {
	QVERIFY(m_receiver.bind(QHostAddress::LocalHost, 0));
	m_port = m_receiver.localPort();
	QVERIFY(m_port != 0);
}

std::vector< byte > TestMediaFraming::roundTripOverUdp(const std::vector< byte > &datagram) {
	m_sender.writeDatagram(reinterpret_cast< const char * >(datagram.data()), static_cast< qint64 >(datagram.size()),
						   QHostAddress::LocalHost, m_port);

	if (!m_receiver.waitForReadyRead(2000)) {
		return {};
	}

	// Buffers are sized from MAX_MEDIA_DATAGRAM_SIZE on both peers, which is the whole point of that
	// constant: the channel is only known after the datagram has been read.
	std::vector< byte > received(MAX_MEDIA_DATAGRAM_SIZE);

	const qint64 read =
		m_receiver.readDatagram(reinterpret_cast< char * >(received.data()), static_cast< qint64 >(received.size()));

	if (read <= 0) {
		return {};
	}

	received.resize(static_cast< std::size_t >(read));

	return received;
}

void TestMediaFraming::audioSurvivesTheChannelPrefix() {
	const std::string key(AES_KEY_SIZE_BYTES, '\x01');
	const std::string ivA(AES_BLOCK_SIZE, '\x02');
	const std::string ivB(AES_BLOCK_SIZE, '\x03');

	CryptStateOCB2 senderCrypt;
	CryptStateOCB2 receiverCrypt;
	QVERIFY(senderCrypt.setKey(key, ivA, ivB));
	QVERIFY(receiverCrypt.setKey(key, ivB, ivA));

	UDPAudioEncoder< Role::Client > encoder(PROTOCOL);
	UDPDecoder< Role::Server > decoder(PROTOCOL);

	AudioData sent;
	// Left unset on purpose. A client does not name itself in an audio packet; the server stamps the
	// session when it relays, exactly as it does for video. Asserting a session here would be asserting
	// something the client-to-server direction never carries.
	sent.frameNumber     = 4242;
	sent.usedCodec       = AudioCodec::Opus;
	sent.targetOrContext = AudioContext::NORMAL;
	sent.isLastFrame     = false;

	const std::vector< byte > opusFrame(120, 0x5A);
	sent.payload = opusFrame;

	const std::span< const byte > encoded = encoder.encodeAudioPacket(sent);
	QVERIFY(!encoded.empty());

	const std::vector< byte > datagram = frameAudio(senderCrypt, encoded);
	QVERIFY(!datagram.empty());

	// The prefix must be the audio channel, and the datagram exactly one byte longer than before.
	QCOMPARE(datagram[0], MEDIA_CHANNEL_AUDIO);
	QCOMPARE(datagram.size(), encoded.size() + 5);

	const std::vector< byte > received = roundTripOverUdp(datagram);
	QVERIFY(received.size() == datagram.size());

	// Now the receive side, exactly as Server::run does it: demux, strip, decrypt, decode.
	QCOMPARE(received[0], MEDIA_CHANNEL_AUDIO);

	std::vector< byte > plaintext(MAX_UDP_PACKET_SIZE);
	QVERIFY(
		receiverCrypt.decrypt(received.data() + 1, plaintext.data(), static_cast< unsigned int >(received.size() - 1)));

	const std::size_t plainLength = received.size() - 1 - 4;

	QVERIFY(decoder.decode(std::span< const byte >(plaintext.data(), plainLength)));
	QCOMPARE(decoder.getMessageType(), UDPMessageType::Audio);

	const AudioData got = decoder.getAudioData();

	// The audio that comes out is the audio that went in. This is the assertion the framing change
	// needed and did not have.
	QCOMPARE(got.senderSession, 0u);
	QCOMPARE(got.frameNumber, sent.frameNumber);
	QCOMPARE(got.usedCodec, sent.usedCodec);
	QCOMPARE(std::vector< byte >(got.payload.begin(), got.payload.end()), opusFrame);
}

void TestMediaFraming::videoSurvivesTheSameSocket() {
	const std::string sessionKey(AES_KEY_SIZE_BYTES, '\x09');

	VideoCryptState senderCrypt;
	VideoCryptState receiverCrypt;
	QVERIFY(senderCrypt.deriveFromSessionKey(sessionKey, true));
	QVERIFY(receiverCrypt.deriveFromSessionKey(sessionKey, false));

	VideoUnitHeader header;
	header.senderSession = SESSION;
	header.streamID      = 2;
	header.frameNumber   = 7;
	header.width         = 128;
	header.height        = 128;
	header.isKeyframe    = true;

	const std::vector< byte > unit(3000, 0x3C);

	VideoFragmenter fragmenter;
	QVERIFY(fragmenter.fragment(header, unit));
	QVERIFY(fragmenter.packets().size() > 1);

	VideoReassembler reassembler;
	VideoUnit assembled;
	bool completed = false;

	for (const std::vector< byte > &packet : fragmenter.packets()) {
		std::vector< byte > datagram;
		QVERIFY(senderCrypt.encrypt(packet, datagram));
		QCOMPARE(datagram[0], MEDIA_CHANNEL_VIDEO);

		const std::vector< byte > received = roundTripOverUdp(datagram);
		QCOMPARE(received.size(), datagram.size());

		std::vector< byte > plaintext;
		QCOMPARE(receiverCrypt.decrypt(received, plaintext), VideoCryptState::Result::Ok);

		if (reassembler.processPacket(plaintext, SESSION, 1, assembled) == VideoReassemblyResult::Complete) {
			completed = true;
		}
	}

	QVERIFY(completed);
	QCOMPARE(assembled.payload, unit);
	QCOMPARE(assembled.header.streamID, header.streamID);
}

void TestMediaFraming::interleavedTrafficIsDemultiplexed() {
	// The case the channel byte exists for: both kinds of datagram arriving on one socket, in any order,
	// each going to the crypt state that can actually decrypt it.
	const std::string key(AES_KEY_SIZE_BYTES, '\x11');
	const std::string ivA(AES_BLOCK_SIZE, '\x12');
	const std::string ivB(AES_BLOCK_SIZE, '\x13');

	CryptStateOCB2 audioSend;
	CryptStateOCB2 audioRecv;
	QVERIFY(audioSend.setKey(key, ivA, ivB));
	QVERIFY(audioRecv.setKey(key, ivB, ivA));

	VideoCryptState videoSend;
	VideoCryptState videoRecv;
	QVERIFY(videoSend.deriveFromSessionKey(key, true));
	QVERIFY(videoRecv.deriveFromSessionKey(key, false));

	UDPAudioEncoder< Role::Client > encoder(PROTOCOL);
	UDPDecoder< Role::Server > decoder(PROTOCOL);

	unsigned int audioDelivered = 0;
	unsigned int videoDelivered = 0;

	for (int i = 0; i < 40; ++i) {
		std::vector< byte > datagram;

		const bool isVideo = (i % 3) == 0;

		if (isVideo) {
			QVERIFY(videoSend.encrypt(std::vector< byte >(400, static_cast< byte >(i)), datagram));
		} else {
			AudioData data;
			data.senderSession   = SESSION;
			data.frameNumber     = static_cast< std::uint64_t >(i);
			data.usedCodec       = AudioCodec::Opus;
			data.targetOrContext = AudioContext::NORMAL;

			const std::vector< byte > frame(80, static_cast< byte >(i));
			data.payload = frame;

			datagram = frameAudio(audioSend, encoder.encodeAudioPacket(data));
			QVERIFY(!datagram.empty());
		}

		const std::vector< byte > received = roundTripOverUdp(datagram);
		QCOMPARE(received.size(), datagram.size());

		if (received[0] == MEDIA_CHANNEL_VIDEO) {
			std::vector< byte > plaintext;
			QCOMPARE(videoRecv.decrypt(received, plaintext), VideoCryptState::Result::Ok);
			videoDelivered++;
		} else {
			QCOMPARE(received[0], MEDIA_CHANNEL_AUDIO);

			std::vector< byte > plaintext(MAX_UDP_PACKET_SIZE);
			QVERIFY(audioRecv.decrypt(received.data() + 1, plaintext.data(),
									  static_cast< unsigned int >(received.size() - 1)));

			QVERIFY(decoder.decode(std::span< const byte >(plaintext.data(), received.size() - 5)));
			QCOMPARE(decoder.getMessageType(), UDPMessageType::Audio);
			QCOMPARE(decoder.getAudioData().frameNumber, static_cast< std::uint64_t >(i));
			audioDelivered++;
		}
	}

	// Every packet of both kinds arrived and decrypted. Before the split, interleaving this much video
	// into audio's sequence would have cost audio packets.
	QCOMPARE(audioDelivered + videoDelivered, 40u);
	QVERIFY(audioDelivered > 0);
	QVERIFY(videoDelivered > 0);
}

void TestMediaFraming::aMaximumSizedVideoDatagramIsNotTruncated() {
	// The bug this guards against is silent: a receive buffer sized for audio would clip a full-size
	// video datagram, and a clipped datagram fails authentication, which on the wire is indistinguishable
	// from packet loss.
	const std::string sessionKey(AES_KEY_SIZE_BYTES, '\x21');

	VideoCryptState send;
	VideoCryptState recv;
	QVERIFY(send.deriveFromSessionKey(sessionKey, true));
	QVERIFY(recv.deriveFromSessionKey(sessionKey, false));

	std::vector< byte > datagram;
	QVERIFY(send.encrypt(std::vector< byte >(MAX_VIDEO_PLAINTEXT_SIZE, 0x77), datagram));
	QCOMPARE(datagram.size(), MAX_VIDEO_DATAGRAM_SIZE);

	// Larger than the old audio-only buffer, which is exactly why both peers had to be resized.
	QVERIFY(datagram.size() > MAX_UDP_PACKET_SIZE);

	const std::vector< byte > received = roundTripOverUdp(datagram);
	QCOMPARE(received.size(), datagram.size());

	std::vector< byte > plaintext;
	QCOMPARE(recv.decrypt(received, plaintext), VideoCryptState::Result::Ok);
	QCOMPARE(plaintext.size(), MAX_VIDEO_PLAINTEXT_SIZE);
}

QTEST_MAIN(TestMediaFraming)
#include "TestMediaFraming.moc"
