// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

// End-to-end video call against a real server process.
//
// Everything else in the suite tests a layer in isolation. This starts the actual mumble-server binary,
// connects real TLS clients that speak the real protocol, and pushes video between them. It exists
// because the three worst bugs found in this area were all invisible to unit tests:
//
//   - the server could not generate its certificate at all, so no TLS connection could ever succeed;
//   - the UDP receive path still read at most 1024 bytes after the video MTU was raised to 1200, so
//     every fragment larger than the old limit was truncated and silently failed authentication;
//   - a subscription could be confirmed while no video actually flowed.
//
// None of those are visible without a server, a socket and two clients.

#include "Mumble.pb.h"
#include "MumbleProtocol.h"
#include "VideoFragmentation.h"
#include "VideoTransport.h"
#include "crypto/CryptStateOCB2.h"

#include <QElapsedTimer>
#include <QObject>
#include <QProcess>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>
#include <QtNetwork/QSslSocket>
#include <QtNetwork/QUdpSocket>
#include <QtTest>

#include <cstdint>
#include <vector>

using namespace Mumble::Protocol;

namespace {

constexpr Version::full_t CLIENT_PROTOCOL = Version::fromComponents(1, 5, 0);

/// A minimal but genuine Mumble client: real TLS, real message framing, real crypto.
struct TestClient {
	QSslSocket tcp;
	QUdpSocket udp;
	CryptStateOCB2 audio;
	VideoCryptState video;
	unsigned int session = 0;
	QByteArray rx;
	quint16 port = 0;

	void sendTcp(const google::protobuf::Message &message, TCPMessageType type) {
		std::string body;
		message.SerializeToString(&body);

		QByteArray packet;
		packet.resize(static_cast< int >(6 + body.size()));

		auto *raw                               = reinterpret_cast< unsigned char * >(packet.data());
		*reinterpret_cast< quint16 * >(raw)     = qToBigEndian(static_cast< quint16 >(type));
		*reinterpret_cast< quint32 * >(raw + 2) = qToBigEndian(static_cast< quint32 >(body.size()));
		memcpy(raw + 6, body.data(), body.size());

		tcp.write(packet);
		tcp.flush();
	}

	/// Reads until a message of the wanted type arrives, handling the ones that set up crypto on the way.
	bool waitFor(TCPMessageType want, QByteArray &out, int milliseconds = 5000) {
		QElapsedTimer timer;
		timer.start();

		while (timer.elapsed() < milliseconds) {
			if (tcp.waitForReadyRead(200)) {
				rx.append(tcp.readAll());
			}

			while (rx.size() >= 6) {
				const auto *raw    = reinterpret_cast< const unsigned char * >(rx.constData());
				const quint16 type = qFromBigEndian< quint16 >(raw);
				const quint32 len  = qFromBigEndian< quint32 >(raw + 2);

				if (static_cast< quint32 >(rx.size()) < 6 + len) {
					break;
				}

				const QByteArray body = rx.mid(6, static_cast< qsizetype >(len));
				rx.remove(0, static_cast< qsizetype >(6 + len));

				if (type == static_cast< quint16 >(TCPMessageType::CryptSetup)) {
					MumbleProto::CryptSetup setup;
					setup.ParseFromArray(body.constData(), static_cast< int >(body.size()));
					audio.setKey(setup.key(), setup.client_nonce(), setup.server_nonce());
					// Video needs no key exchange of its own; both ends derive from the session key.
					video.deriveFromSessionKey(setup.key(), false);
				}

				if (type == static_cast< quint16 >(TCPMessageType::ServerSync)) {
					MumbleProto::ServerSync sync;
					sync.ParseFromArray(body.constData(), static_cast< int >(body.size()));
					session = sync.session();
				}

				if (type == static_cast< quint16 >(want)) {
					out = body;

					return true;
				}
			}
		}

		return false;
	}

	bool connectAndAuthenticate(const char *username, quint16 serverPort) {
		port = serverPort;

		tcp.setPeerVerifyMode(QSslSocket::VerifyNone);
		tcp.connectToHostEncrypted(QStringLiteral("127.0.0.1"), serverPort);

		if (!tcp.waitForEncrypted(10000)) {
			return false;
		}

		if (!udp.bind(QHostAddress::LocalHost, 0)) {
			return false;
		}

		MumbleProto::Version version;
		version.set_version_v2(CLIENT_PROTOCOL);
		version.set_release("TestVideoCall");
		version.set_os("linux");
		version.set_os_version("1");
		sendTcp(version, TCPMessageType::Version);

		MumbleProto::Authenticate authenticate;
		authenticate.set_username(username);
		authenticate.set_opus(true);
		sendTcp(authenticate, TCPMessageType::Authenticate);

		QByteArray body;

		return waitFor(TCPMessageType::ServerSync, body);
	}

	/// The server binds a UDP peer to a user only once it has decrypted something from them, so a client
	/// that has never spoken on the audio channel cannot receive video either.
	void establishUdpPath() {
		UDPPingEncoder< Role::Client > encoder(CLIENT_PROTOCOL);

		PingData ping;
		ping.timestamp = 1;

		const std::span< const byte > encoded = encoder.encodePingPacket(ping);

		std::vector< byte > datagram(encoded.size() + 5);
		datagram[0] = MEDIA_CHANNEL_AUDIO;
		audio.encrypt(encoded.data(), datagram.data() + 1, static_cast< unsigned int >(encoded.size()));

		udp.writeDatagram(reinterpret_cast< const char * >(datagram.data()), static_cast< qint64 >(datagram.size()),
						  QHostAddress::LocalHost, port);
	}

	/// Collects video datagrams for a while, returning how many decrypted successfully.
	int drainVideo(VideoReassembler &reassembler, std::uint32_t senderSession, VideoUnit &unit, bool &complete,
				   int milliseconds) {
		QElapsedTimer timer;
		timer.start();

		int received = 0;

		while (timer.elapsed() < milliseconds) {
			if (!udp.waitForReadyRead(150)) {
				continue;
			}

			while (udp.hasPendingDatagrams()) {
				std::vector< byte > buffer(MAX_MEDIA_DATAGRAM_SIZE);

				const qint64 read =
					udp.readDatagram(reinterpret_cast< char * >(buffer.data()), static_cast< qint64 >(buffer.size()));

				if (read <= 0) {
					continue;
				}

				buffer.resize(static_cast< std::size_t >(read));

				if (buffer[0] != MEDIA_CHANNEL_VIDEO) {
					continue;
				}

				received++;

				std::vector< byte > plaintext;

				if (video.decrypt(buffer, plaintext) != VideoCryptState::Result::Ok) {
					continue;
				}

				if (reassembler.processPacket(plaintext, senderSession, static_cast< std::uint64_t >(timer.elapsed()),
											  unit)
					== VideoReassemblyResult::Complete) {
					complete = true;
				}
			}
		}

		return received;
	}
};

} // namespace

class TestVideoCall : public QObject {
	Q_OBJECT

private slots:
	void initTestCase();
	void cleanupTestCase();

	void aVideoCallWorksEndToEnd();
	void anUnsubscribedClientReceivesNothing();

private:
	QTemporaryDir m_dir;
	QProcess m_server;
	quint16 m_port = 0;

	bool startServer();
};

bool TestVideoCall::startServer() {
	// A port the kernel just told us was free, so parallel test runs do not collide.
	{
		QTcpServer probe;

		if (!probe.listen(QHostAddress::LocalHost, 0)) {
			return false;
		}

		m_port = probe.serverPort();
	}

	const QString ini = m_dir.filePath(QStringLiteral("mumble-server.ini"));

	QFile file(ini);

	if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
		return false;
	}

	QTextStream out(&file);
	out << "database=" << m_dir.filePath(QStringLiteral("server.sqlite")) << "\n";
	out << "logfile=" << m_dir.filePath(QStringLiteral("server.log")) << "\n";
	out << "host=127.0.0.1\n";
	out << "port=" << m_port << "\n";
	out << "users=10\n";
	// Nothing here should reach the network or a real registry.
	out << "registername=\n";
	out << "registerurl=\n";
	file.close();

	m_server.setProgram(QStringLiteral(MUMBLE_SERVER_BINARY));
	m_server.setArguments({ QStringLiteral("-i"), ini });
	m_server.setWorkingDirectory(m_dir.path());
	m_server.start();

	if (!m_server.waitForStarted(10000)) {
		return false;
	}

	// Wait for it to actually accept connections rather than sleeping a guessed interval.
	QElapsedTimer timer;
	timer.start();

	while (timer.elapsed() < 30000) {
		if (m_server.state() != QProcess::Running) {
			return false;
		}

		QSslSocket probe;
		probe.setPeerVerifyMode(QSslSocket::VerifyNone);
		probe.connectToHostEncrypted(QStringLiteral("127.0.0.1"), m_port);

		if (probe.waitForEncrypted(1000)) {
			probe.abort();

			return true;
		}

		QThread::msleep(250);
	}

	return false;
}

void TestVideoCall::initTestCase() {
	QVERIFY(m_dir.isValid());

	if (!QFile::exists(QStringLiteral(MUMBLE_SERVER_BINARY))) {
		QSKIP("mumble-server was not built");
	}

	QVERIFY2(startServer(), "the server did not come up; see the log in the temporary directory");
}

void TestVideoCall::cleanupTestCase() {
	if (m_server.state() != QProcess::NotRunning) {
		m_server.terminate();

		if (!m_server.waitForFinished(5000)) {
			m_server.kill();
			m_server.waitForFinished(5000);
		}
	}
}

void TestVideoCall::aVideoCallWorksEndToEnd() {
	TestClient alice;
	TestClient bob;

	QVERIFY2(alice.connectAndAuthenticate("alice", m_port), "alice could not authenticate");
	QVERIFY2(bob.connectAndAuthenticate("bob", m_port), "bob could not authenticate");
	QVERIFY(alice.session != bob.session);

	alice.establishUdpPath();
	bob.establishUdpPath();
	QThread::msleep(500);

	MumbleProto::VideoState announcement;
	announcement.set_stream_id(0);
	announcement.set_active(true);
	announcement.set_codec(MumbleProto::VideoState_Codec_TiledImage);
	announcement.set_source_kind(MumbleProto::VideoState_SourceKind_Camera);
	alice.sendTcp(announcement, TCPMessageType::VideoState);

	QByteArray body;
	QVERIFY2(bob.waitFor(TCPMessageType::VideoState, body), "the announcement was not relayed to bob");

	MumbleProto::VideoState relayed;
	QVERIFY(relayed.ParseFromArray(body.constData(), static_cast< int >(body.size())));

	// The server owns this field. If a client could set it, anyone could announce a stream in someone
	// else's name.
	QCOMPARE(relayed.session(), alice.session);

	MumbleProto::VideoSubscribe subscribe;
	subscribe.set_session(alice.session);
	subscribe.set_stream_id(0);
	subscribe.set_subscribe(true);
	bob.sendTcp(subscribe, TCPMessageType::VideoSubscribe);

	QByteArray confirmation;
	QVERIFY2(bob.waitFor(TCPMessageType::VideoSubscribe, confirmation), "the subscription was not confirmed");

	// Deliberately larger than one datagram, so this exercises fragmentation, the per-recipient
	// re-encryption and reassembly rather than a single lucky packet.
	std::vector< byte > payload(4000);

	for (std::size_t i = 0; i < payload.size(); ++i) {
		payload[i] = static_cast< byte >((i * 7) & 0xFF);
	}

	VideoUnitHeader header;
	header.streamID    = 0;
	header.frameNumber = 1;
	header.unitID      = 0;
	header.isKeyframe  = true;
	header.isFrameEnd  = true;
	header.width       = 640;
	header.height      = 480;

	VideoFragmenter fragmenter;
	QVERIFY(fragmenter.fragment(header, payload));
	QVERIFY(fragmenter.packets().size() > 1);

	for (const std::vector< byte > &packet : fragmenter.packets()) {
		std::vector< byte > datagram;
		QVERIFY(alice.video.encrypt(packet, datagram));

		// Every fragment but the last is full sized. These are the ones that were being truncated when
		// the receive path still read at most MAX_UDP_PACKET_SIZE.
		QVERIFY(datagram.size() <= MAX_VIDEO_DATAGRAM_SIZE);

		alice.udp.writeDatagram(reinterpret_cast< const char * >(datagram.data()),
								static_cast< qint64 >(datagram.size()), QHostAddress::LocalHost, m_port);
	}

	VideoReassembler reassembler;
	VideoUnit unit;
	bool complete = false;

	const int received = bob.drainVideo(reassembler, alice.session, unit, complete, 5000);

	QVERIFY2(received > 0, "bob received no video datagrams at all");
	QCOMPARE(static_cast< std::size_t >(received), fragmenter.packets().size());
	QVERIFY2(complete, "the unit never reassembled");

	QCOMPARE(unit.payload, payload);
	QCOMPARE(unit.header.senderSession, alice.session);
	QCOMPARE(unit.header.width, 640u);
	QCOMPARE(unit.header.height, 480u);
}

void TestVideoCall::anUnsubscribedClientReceivesNothing() {
	// Routing is subscription-based, and this is the property that makes that a security boundary rather
	// than a bandwidth optimisation: being present on the server, and knowing the sender's session id
	// from the user list, must not be enough to receive someone's camera.
	TestClient alice;
	TestClient carol;

	QVERIFY(alice.connectAndAuthenticate("alice2", m_port));
	QVERIFY(carol.connectAndAuthenticate("carol", m_port));

	alice.establishUdpPath();
	carol.establishUdpPath();
	QThread::msleep(500);

	MumbleProto::VideoState announcement;
	announcement.set_stream_id(0);
	announcement.set_active(true);
	alice.sendTcp(announcement, TCPMessageType::VideoState);

	QByteArray body;
	QVERIFY(carol.waitFor(TCPMessageType::VideoState, body));

	// Carol deliberately does not subscribe.

	std::vector< byte > payload(2000, 0x5A);

	VideoUnitHeader header;
	header.streamID    = 0;
	header.frameNumber = 1;
	header.isKeyframe  = true;

	VideoFragmenter fragmenter;
	QVERIFY(fragmenter.fragment(header, payload));

	for (const std::vector< byte > &packet : fragmenter.packets()) {
		std::vector< byte > datagram;
		QVERIFY(alice.video.encrypt(packet, datagram));
		alice.udp.writeDatagram(reinterpret_cast< const char * >(datagram.data()),
								static_cast< qint64 >(datagram.size()), QHostAddress::LocalHost, m_port);
	}

	VideoReassembler reassembler;
	VideoUnit unit;
	bool complete = false;

	const int received = carol.drainVideo(reassembler, alice.session, unit, complete, 1500);

	QCOMPARE(received, 0);
	QVERIFY(!complete);
}

QTEST_MAIN(TestVideoCall)
#include "TestVideoCall.moc"
