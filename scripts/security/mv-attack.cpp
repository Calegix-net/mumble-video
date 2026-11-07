// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

// A headless, scriptable Mumble client for security testing.
//
// It connects to a server exactly as a real client does - TLS control channel, the Version /
// Authenticate / CryptSetup / ServerSync handshake, then UDP media keyed from the session key - and
// then does whatever its flags say, including things a conforming client never would. It links
// mumble-core, so the framing and the OCB2 video crypto are the real implementation rather than a
// reimplementation that might paper over the very bug under test.
//
// It is a verification tool for a server you run. Everything it does is something an ordinary
// authenticated user of that server could do; it does not attempt to forge authentication, break the
// cipher, or reach any machine but the one named on the command line. It exists to turn "I read the
// code and believe X" into "X was observed", and to be the regression test for the fixes already made.
//
// Output is one JSON object per line on stdout, so an orchestrator can assert on events rather than
// scrape prose. Diagnostics go to stderr.

#include "mumble-core/MumbleCore.h"

#include "Mumble.pb.h"
#include "MumbleUDP.pb.h"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace {

using MumbleCore::Bytes;
using MumbleCore::ByteView;

// ---------------------------------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------------------------------

struct Options {
	std::string host = "127.0.0.1";
	std::uint16_t port = 64740;
	std::string name = "attacker";
	std::string certPath; // PEM cert the client presents; optional (servers take a self-signed one).
	std::string keyPath;

	std::string role = "idle"; // idle | sender | watcher

	// Sender knobs.
	std::string codec = "opusaudio"; // opusaudio | vp9 | tiledimage | h264 | vp8
	std::uint32_t streamID = 1;
	bool selfMute = false;
	bool selfDeaf = false;
	std::size_t sourceNameBytes = 0; // 0 = a short real-looking name
	unsigned int rateHz = 30;
	std::size_t unitBytes = 200; // coded bytes per unit

	// Watcher knobs.
	std::uint32_t watchSession = 0; // whose stream to subscribe to
	std::uint32_t watchStream = 1;

	unsigned int durationMs = 4000;
};

[[noreturn]] void usage(const char *argv0) {
	std::fprintf(stderr,
				 "usage: %s --host H --port P --name N [--cert c.pem --key k.pem] --role idle|sender|watcher\n"
				 "  sender:  --codec opusaudio|vp9|tiledimage|h264|vp8 --stream-id N [--self-mute] [--self-deaf]\n"
				 "           [--source-name-bytes N] [--rate-hz N] [--unit-bytes N]\n"
				 "  watcher: --watch-session N --watch-stream N [--self-deaf]\n"
				 "  both:    --duration-ms N\n",
				 argv0);
	std::exit(2);
}

Options parseArgs(int argc, char **argv) {
	Options o;
	auto need = [&](int &i) -> const char * {
		if (i + 1 >= argc) {
			usage(argv[0]);
		}
		return argv[++i];
	};
	for (int i = 1; i < argc; ++i) {
		const std::string a = argv[i];
		if (a == "--host") {
			o.host = need(i);
		} else if (a == "--port") {
			o.port = static_cast< std::uint16_t >(std::atoi(need(i)));
		} else if (a == "--name") {
			o.name = need(i);
		} else if (a == "--cert") {
			o.certPath = need(i);
		} else if (a == "--key") {
			o.keyPath = need(i);
		} else if (a == "--role") {
			o.role = need(i);
		} else if (a == "--codec") {
			o.codec = need(i);
		} else if (a == "--stream-id") {
			o.streamID = static_cast< std::uint32_t >(std::strtoul(need(i), nullptr, 10));
		} else if (a == "--self-mute") {
			o.selfMute = true;
		} else if (a == "--self-deaf") {
			o.selfDeaf = true;
		} else if (a == "--source-name-bytes") {
			o.sourceNameBytes = static_cast< std::size_t >(std::strtoul(need(i), nullptr, 10));
		} else if (a == "--rate-hz") {
			o.rateHz = static_cast< unsigned int >(std::strtoul(need(i), nullptr, 10));
		} else if (a == "--unit-bytes") {
			o.unitBytes = static_cast< std::size_t >(std::strtoul(need(i), nullptr, 10));
		} else if (a == "--watch-session") {
			o.watchSession = static_cast< std::uint32_t >(std::strtoul(need(i), nullptr, 10));
		} else if (a == "--watch-stream") {
			o.watchStream = static_cast< std::uint32_t >(std::strtoul(need(i), nullptr, 10));
		} else if (a == "--duration-ms") {
			o.durationMs = static_cast< unsigned int >(std::strtoul(need(i), nullptr, 10));
		} else {
			std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
			usage(argv[0]);
		}
	}
	return o;
}

// ---------------------------------------------------------------------------------------------------
// JSON event output. A deliberately tiny emitter: these are flat objects with string and integer
// values, so a real JSON library would be more machinery than the job needs.
// ---------------------------------------------------------------------------------------------------

std::string jsonEscape(const std::string &s) {
	std::string out;
	for (char c : s) {
		if (c == '"' || c == '\\') {
			out.push_back('\\');
			out.push_back(c);
		} else if (static_cast< unsigned char >(c) < 0x20) {
			char buf[8];
			std::snprintf(buf, sizeof(buf), "\\u%04x", c);
			out += buf;
		} else {
			out.push_back(c);
		}
	}
	return out;
}

void emitEvent(const std::string &event, const std::map< std::string, std::string > &strs,
			   const std::map< std::string, long long > &nums) {
	std::string line = "{\"event\":\"" + jsonEscape(event) + "\"";
	for (const auto &kv : strs) {
		line += ",\"" + jsonEscape(kv.first) + "\":\"" + jsonEscape(kv.second) + "\"";
	}
	for (const auto &kv : nums) {
		line += ",\"" + jsonEscape(kv.first) + "\":" + std::to_string(kv.second);
	}
	line += "}";
	std::printf("%s\n", line.c_str());
	std::fflush(stdout);
}

std::uint64_t nowUsec() {
	return static_cast< std::uint64_t >(
		std::chrono::duration_cast< std::chrono::microseconds >(std::chrono::steady_clock::now().time_since_epoch())
			.count());
}

// ---------------------------------------------------------------------------------------------------
// TLS control-channel transport. No peer verification: a Mumble client trusts a server's certificate on
// first sight by its hash, so a CA check is not part of the protocol, and this is a test client talking
// to a server whose identity the orchestrator already established out of band.
// ---------------------------------------------------------------------------------------------------

class TlsConnection {
public:
	bool connect(const Options &o) {
		SSL_library_init();
		SSL_load_error_strings();
		m_ctx = SSL_CTX_new(TLS_client_method());
		if (!m_ctx) {
			return fail("SSL_CTX_new");
		}
		if (!o.certPath.empty() && !o.keyPath.empty()) {
			if (SSL_CTX_use_certificate_file(m_ctx, o.certPath.c_str(), SSL_FILETYPE_PEM) != 1
				|| SSL_CTX_use_PrivateKey_file(m_ctx, o.keyPath.c_str(), SSL_FILETYPE_PEM) != 1) {
				return fail("loading client certificate");
			}
		}

		m_fd = ::socket(AF_INET, SOCK_STREAM, 0);
		if (m_fd < 0) {
			return fail("socket");
		}
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port   = htons(o.port);
		if (::inet_pton(AF_INET, o.host.c_str(), &addr.sin_addr) != 1) {
			return fail("inet_pton");
		}
		if (::connect(m_fd, reinterpret_cast< sockaddr * >(&addr), sizeof(addr)) != 0) {
			return fail("connect");
		}

		m_ssl = SSL_new(m_ctx);
		SSL_set_fd(m_ssl, m_fd);
		if (SSL_connect(m_ssl) != 1) {
			return fail("SSL_connect");
		}
		return true;
	}

	bool write(ByteView bytes) {
		int n = SSL_write(m_ssl, bytes.data(), static_cast< int >(bytes.size()));
		return n == static_cast< int >(bytes.size());
	}

	// Non-blocking-ish read: returns what is available now, or empty if nothing. The caller drives a
	// select() loop, so this is only called when the fd is readable.
	Bytes readSome() {
		unsigned char buf[16384];
		int n = SSL_read(m_ssl, buf, sizeof(buf));
		if (n <= 0) {
			return {};
		}
		return Bytes(buf, buf + n);
	}

	int fd() const { return m_fd; }

	~TlsConnection() {
		if (m_ssl) {
			SSL_shutdown(m_ssl);
			SSL_free(m_ssl);
		}
		if (m_fd >= 0) {
			::close(m_fd);
		}
		if (m_ctx) {
			SSL_CTX_free(m_ctx);
		}
	}

private:
	bool fail(const char *what) {
		std::fprintf(stderr, "tls: %s failed: %s\n", what, ERR_error_string(ERR_get_error(), nullptr));
		return false;
	}

	SSL_CTX *m_ctx = nullptr;
	SSL *m_ssl     = nullptr;
	int m_fd       = -1;
};

// ---------------------------------------------------------------------------------------------------
// UDP transport.
// ---------------------------------------------------------------------------------------------------

class UdpSocket {
public:
	bool open(const Options &o) {
		m_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
		if (m_fd < 0) {
			return false;
		}
		m_addr.sin_family = AF_INET;
		m_addr.sin_port   = htons(o.port);
		return ::inet_pton(AF_INET, o.host.c_str(), &m_addr.sin_addr) == 1;
	}

	void send(ByteView datagram) {
		::sendto(m_fd, datagram.data(), datagram.size(), 0, reinterpret_cast< sockaddr * >(&m_addr),
				 sizeof(m_addr));
	}

	Bytes recv() {
		unsigned char buf[2048];
		ssize_t n = ::recvfrom(m_fd, buf, sizeof(buf), 0, nullptr, nullptr);
		if (n <= 0) {
			return {};
		}
		return Bytes(buf, buf + n);
	}

	int fd() const { return m_fd; }

	~UdpSocket() {
		if (m_fd >= 0) {
			::close(m_fd);
		}
	}

private:
	int m_fd = -1;
	sockaddr_in m_addr{};
};

// ---------------------------------------------------------------------------------------------------
// The client.
// ---------------------------------------------------------------------------------------------------

int videoCodecValue(const std::string &name) {
	if (name == "opusaudio") {
		return MumbleProto::VideoState_Codec_OpusAudio;
	}
	if (name == "vp9") {
		return MumbleProto::VideoState_Codec_VP9;
	}
	if (name == "vp8") {
		return MumbleProto::VideoState_Codec_VP8;
	}
	if (name == "tiledimage") {
		return MumbleProto::VideoState_Codec_TiledImage;
	}
	if (name == "h264") {
		return MumbleProto::VideoState_Codec_H264;
	}
	return MumbleProto::VideoState_Codec_CODEC_UNKNOWN;
}

class Client {
public:
	explicit Client(const Options &o) : m_o(o) {}

	int run() {
		if (!m_tls.connect(m_o) || !m_udp.open(m_o)) {
			emitEvent("fatal", { { "why", "connect" } }, {});
			return 1;
		}
		sendVersionAndAuthenticate();

		const std::uint64_t deadline = nowUsec() + static_cast< std::uint64_t >(m_o.durationMs) * 1000;
		std::uint64_t nextSendUsec   = 0;
		std::uint64_t nextPingUsec   = 0;

		while (nowUsec() < deadline) {
			fd_set rfds;
			FD_ZERO(&rfds);
			FD_SET(m_tls.fd(), &rfds);
			FD_SET(m_udp.fd(), &rfds);
			int maxfd = std::max(m_tls.fd(), m_udp.fd());

			timeval tv{ 0, 5000 }; // 5 ms, fine-grained enough to pace a 30 Hz sender
			int r = ::select(maxfd + 1, &rfds, nullptr, nullptr, &tv);
			if (r < 0 && errno != EINTR) {
				break;
			}

			if (r > 0 && FD_ISSET(m_tls.fd(), &rfds)) {
				pumpControl();
			}
			if (r > 0 && FD_ISSET(m_udp.fd(), &rfds)) {
				pumpUdp();
			}

			const std::uint64_t t = nowUsec();

			// A keepalive ping, so the server keeps our UDP mapping and marks us UDP-reachable.
			if (m_keyed && t >= nextPingUsec) {
				nextPingUsec = t + 1'000'000;
				if (auto ping = m_session.makePing(t)) {
					m_udp.send(*ping);
				}
			}

			if (m_ready && m_o.role == "sender" && t >= nextSendUsec) {
				nextSendUsec = t + (m_o.rateHz ? 1'000'000 / m_o.rateHz : 1'000'000);
				sendOneUnit();
			}
		}

		reportSummary();
		return 0;
	}

private:
	// --- control channel ---

	void sendControl(std::uint16_t type, const google::protobuf::MessageLite &msg) {
		std::string payload;
		msg.SerializeToString(&payload);
		const Bytes framed = MumbleCore::frameControlMessage(
			type, ByteView(reinterpret_cast< const std::uint8_t * >(payload.data()), payload.size()));
		m_tls.write(framed);
	}

	void sendVersionAndAuthenticate() {
		MumbleProto::Version v;
		v.set_version_v2(0x0001000500000000ULL); // 1.5.0.0, new enough for video
		v.set_release("mv-attack");
		v.set_os("Linux");
		sendControl(MumbleCore::ControlType::Version, v);

		MumbleProto::Authenticate a;
		a.set_username(m_o.name);
		a.set_opus(true);
		a.add_features(MumbleProto::FEATURE_VIDEO);
		sendControl(MumbleCore::ControlType::Authenticate, a);
	}

	void pumpControl() {
		const Bytes bytes = m_tls.readSome();
		if (bytes.empty()) {
			return;
		}
		m_reader.feed(ByteView(bytes.data(), bytes.size()));
		while (auto msg = m_reader.next()) {
			dispatchControl(*msg);
		}
	}

	void dispatchControl(const MumbleCore::ControlMessage &msg) {
		switch (msg.type) {
			case MumbleCore::ControlType::CryptSetup: {
				MumbleCore::MediaEvents ev;
				m_session.handleCryptSetup(ByteView(msg.payload.data(), msg.payload.size()), ev);
				m_keyed = m_session.isKeyed();
				flush(ev);
				break;
			}
			case MumbleCore::ControlType::ServerSync: {
				MumbleProto::ServerSync s;
				if (s.ParseFromArray(msg.payload.data(), static_cast< int >(msg.payload.size()))) {
					m_ownSession = s.session();
					emitEvent("synced", {}, { { "session", m_ownSession } });
					onSynced();
				}
				break;
			}
			case MumbleCore::ControlType::VideoState: {
				MumbleProto::VideoState vs;
				if (vs.ParseFromArray(msg.payload.data(), static_cast< int >(msg.payload.size()))) {
					emitEvent("video_state",
							  { { "source_name_sample", vs.source_name().substr(0, 16) } },
							  { { "from", vs.session() },
								{ "stream", vs.stream_id() },
								{ "active", vs.active() ? 1 : 0 },
								{ "codec", vs.codec() },
								{ "source_name_len", static_cast< long long >(vs.source_name().size()) } });
				}
				break;
			}
			case MumbleCore::ControlType::PermissionDenied: {
				MumbleProto::PermissionDenied pd;
				if (pd.ParseFromArray(msg.payload.data(), static_cast< int >(msg.payload.size()))) {
					emitEvent("permission_denied", { { "reason", pd.reason() } },
							  { { "permission", pd.permission() }, { "type", pd.type() } });
				}
				break;
			}
			case MumbleCore::ControlType::UserState: {
				MumbleProto::UserState us;
				if (us.ParseFromArray(msg.payload.data(), static_cast< int >(msg.payload.size()))
					&& us.session() == m_ownSession) {
					emitEvent("self_state", {},
							  { { "suppress", us.suppress() ? 1 : 0 },
								{ "mute", us.mute() ? 1 : 0 },
								{ "deaf", us.deaf() ? 1 : 0 } });
				}
				break;
			}
			case MumbleCore::ControlType::Reject: {
				emitEvent("rejected", {}, {});
				break;
			}
			default:
				break;
		}
	}

	// Once the server has told us our session, do the role's opening move. Self-mute and self-deaf go
	// up here, before any media: a UserState for our own session carries them, and the server applies
	// them to bSelfMute / bSelfDeaf.
	void onSynced() {
		if (m_o.selfMute || m_o.selfDeaf) {
			MumbleProto::UserState us;
			us.set_session(m_ownSession);
			if (m_o.selfMute) {
				us.set_self_mute(true);
			}
			if (m_o.selfDeaf) {
				us.set_self_deaf(true);
			}
			sendControl(MumbleCore::ControlType::UserState, us);
		}

		if (m_o.role == "sender") {
			announceStream();
		} else if (m_o.role == "watcher") {
			subscribe();
		}

		m_ready = true;
	}

	void announceStream() {
		MumbleProto::VideoState vs;
		vs.set_stream_id(m_o.streamID);
		vs.set_active(true);
		vs.set_codec(static_cast< MumbleProto::VideoState_Codec >(videoCodecValue(m_o.codec)));
		vs.set_source_kind(m_o.codec == "opusaudio" ? MumbleProto::VideoState_SourceKind_SOURCE_UNKNOWN
													: MumbleProto::VideoState_SourceKind_Display);
		if (m_o.sourceNameBytes > 0) {
			vs.set_source_name(std::string(m_o.sourceNameBytes, 'A'));
		} else {
			vs.set_source_name(m_o.codec == "opusaudio" ? "mic" : "Screen 1");
		}
		sendControl(MumbleCore::ControlType::VideoState, vs);
		emitEvent("announced", { { "codec", m_o.codec } },
				  { { "stream", m_o.streamID },
					{ "source_name_bytes", static_cast< long long >(m_o.sourceNameBytes) } });
	}

	void subscribe() {
		MumbleProto::VideoSubscribe sub;
		sub.set_session(m_o.watchSession);
		sub.set_stream_id(m_o.watchStream);
		sub.set_subscribe(true);
		sendControl(MumbleCore::ControlType::VideoSubscribe, sub);
		emitEvent("subscribed", {}, { { "to", m_o.watchSession }, { "stream", m_o.watchStream } });
	}

	// --- media ---

	void sendOneUnit() {
		MumbleCore::VideoUnit unit;
		unit.streamID             = m_o.streamID;
		unit.frameNumber          = ++m_frame;
		unit.unitID               = 0;
		unit.isKeyframe           = (m_frame == 1);
		unit.isFrameEnd           = true;
		unit.captureTimestampUsec = nowUsec();
		unit.width                = 1280;
		unit.height               = 720;
		// A payload the server will relay. Content does not matter to the routing decision under test.
		unit.payload.assign(m_o.unitBytes, static_cast< std::uint8_t >(0x40 + (m_frame & 0x1f)));

		std::vector< Bytes > datagrams = m_session.makeVideo(unit);
		if (datagrams.empty()) {
			return;
		}

		for (Bytes &d : datagrams) {
			m_udp.send(d);
			m_sentUnits++;
			m_sentBytes += d.size();
		}
	}

	void pumpUdp() {
		const Bytes datagram = m_udp.recv();
		if (datagram.empty()) {
			return;
		}
		m_recvBytes += datagram.size();

		MumbleCore::MediaEvents ev;
		m_session.receiveDatagram(ByteView(datagram.data(), datagram.size()), nowUsec(), ev);
		flush(ev);
	}

	void flush(MumbleCore::MediaEvents &ev) {
		for (const Bytes &d : ev.datagramsToSend) {
			m_udp.send(d);
		}
		for (const Bytes &c : ev.controlToSend) {
			m_tls.write(c);
		}
		for (const MumbleCore::VideoUnit &u : ev.video) {
			m_recvUnits++;
			if (m_recvUnitsLogged < 3) {
				emitEvent("recv_video_unit", {},
						  { { "from", u.senderSession }, { "stream", u.streamID }, { "frame",
							static_cast< long long >(u.frameNumber) } });
				m_recvUnitsLogged++;
			}
		}
		ev.clear();
	}

	void reportSummary() {
		emitEvent("summary", { { "role", m_o.role } },
				  { { "own_session", m_ownSession },
					{ "sent_units", static_cast< long long >(m_sentUnits) },
					{ "sent_bytes", static_cast< long long >(m_sentBytes) },
					{ "recv_units", static_cast< long long >(m_recvUnits) },
					{ "recv_bytes", static_cast< long long >(m_recvBytes) } });
	}

	Options m_o;
	TlsConnection m_tls;
	UdpSocket m_udp;
	MumbleCore::ControlReader m_reader;
	MumbleCore::MediaSession m_session;

	std::uint32_t m_ownSession = 0;
	bool m_keyed               = false;
	bool m_ready               = false;

	std::uint64_t m_frame      = 0;
	long long m_sentUnits      = 0;
	long long m_sentBytes      = 0;
	long long m_recvUnits      = 0;
	long long m_recvBytes      = 0;
	int m_recvUnitsLogged      = 0;
};

} // namespace

int main(int argc, char **argv) {
	GOOGLE_PROTOBUF_VERIFY_VERSION;
	const Options o = parseArgs(argc, argv);
	Client client(o);
	return client.run();
}
