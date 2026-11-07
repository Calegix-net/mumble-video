// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_VIDEOTRANSPORT_H_
#define MUMBLE_VIDEOTRANSPORT_H_

#include "MumbleProtocol.h"
#include "crypto/CryptStateOCB2.h"

#include <bitset>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Mumble {
namespace Protocol {

	using byte = std::uint8_t;

	// First byte of every media datagram, in the clear, identifying which stream it belongs to.
	//
	// Audio and video share one UDP socket - a second port would mean a second hole to open in every
	// firewall and a second NAT binding to keep alive, for no benefit - but they must not share a
	// sequence space. This byte is what lets the receiver pick the right crypt state before it can
	// decrypt anything.
	constexpr byte MEDIA_CHANNEL_AUDIO = 0x00;
	constexpr byte MEDIA_CHANNEL_VIDEO = 0x01;

	// Largest video datagram we will put on the wire, including every header below.
	//
	// Not MAX_UDP_PACKET_SIZE. That constant is 1024, sized for Opus frames long before anything else
	// shared the socket, and inheriting it cost roughly a third of every video packet to headers. 1200
	// is the largest size that still fits inside IPv6's guaranteed minimum MTU of 1280 after a 40-byte
	// IPv6 header and an 8-byte UDP header, with margin left for a tunnel or VPN encapsulation. It is
	// the same number QUIC uses as its floor, and for the same reason: there is no path MTU discovery
	// here, so the safe value is the one that works without it.
	constexpr std::size_t MAX_VIDEO_DATAGRAM_SIZE = 1200;

	// Datagram layout:
	//   [0]      channel byte, in the clear
	//   [1..4]   32-bit big-endian sequence number, in the clear
	//   [5..7]   first three bytes of the OCB2 authentication tag
	//   [8..]    ciphertext
	constexpr std::size_t VIDEO_SEQUENCE_OFFSET = 1;
	constexpr std::size_t VIDEO_TAG_OFFSET      = 5;
	constexpr std::size_t VIDEO_TAG_SIZE        = 3;
	constexpr std::size_t VIDEO_HEADER_SIZE     = 8;

	constexpr std::size_t MAX_VIDEO_PLAINTEXT_SIZE = MAX_VIDEO_DATAGRAM_SIZE - VIDEO_HEADER_SIZE;

	// An audio datagram is the old OCB2 payload with the channel byte in front of it.
	constexpr std::size_t MAX_AUDIO_DATAGRAM_SIZE = MAX_UDP_PACKET_SIZE + 1;

	// What a receive buffer has to be able to hold, whichever channel a datagram turns out to belong to.
	// The two are demultiplexed only after the datagram has been read, so the buffer must fit the larger.
	constexpr std::size_t MAX_MEDIA_DATAGRAM_SIZE =
		MAX_VIDEO_DATAGRAM_SIZE > MAX_AUDIO_DATAGRAM_SIZE ? MAX_VIDEO_DATAGRAM_SIZE : MAX_AUDIO_DATAGRAM_SIZE;

	// One byte of the plaintext is the UDP message type. It is redundant with the channel byte, and kept
	// only so that a decrypted video datagram is still self-describing to the existing decoder shape.
	constexpr std::size_t MAX_VIDEO_PROTOBUF_SIZE = MAX_VIDEO_PLAINTEXT_SIZE - 1;

	// How far behind the newest accepted packet a straggler may be and still be accepted.
	//
	// This replaces the fixed 30-packet tolerance that comes from OCB2's one-byte rolling IV, which is
	// what previously made a burst of more than 30 fragments undeliverable if the network reordered it.
	// 512 is comfortably more than the largest unit's fragment count (MAX_VIDEO_FRAGMENTS_PER_UNIT, 256),
	// so a whole unit arriving in reverse order still decrypts. An endpoint with a smaller window - a server
	// built before this was raised - only rejects packets reordered by more than its own window, which an
	// in-order burst never is.
	constexpr std::size_t VIDEO_REPLAY_WINDOW = 512;

	// Sequence 0 is never sent, so a receiver can use "highest seen == 0" to mean "nothing yet".
	constexpr std::uint32_t VIDEO_FIRST_SEQUENCE = 1;

	// How far ahead of everything accepted so far a sequence may be and still be taken at face value.
	//
	// This exists because the authentication tag is three bytes. A blind forgery therefore succeeds about
	// once in 16.7 million attempts, which sounds remote until you notice it costs the attacker one UDP
	// packet per attempt - and that the replay window anchors itself to the highest sequence ever
	// accepted. Without a bound, one successful forgery carrying a sequence near the top of the 32-bit
	// space moved that anchor there, and every genuine packet afterwards was rejected as TooOld for the
	// rest of the connection. One lucky packet, video dead, no error anywhere. MumbleFuzzcrypt
	// demonstrated it: see corpus/crypt/crypt-forged-sequence-jump.
	//
	// The size has to be read against VIDEO_REPLAY_WINDOW, and a generous value is no use at all.
	//
	// A forged packet accepted at the anchor plus N leaves every genuine packet still in flight N behind
	// the anchor, and anything VIDEO_REPLAY_WINDOW or more behind is refused as TooOld. So a bound
	// larger than the window does not prevent the lockout, it only changes how far the forger has to
	// guess - which was the first mistake made here: 65536 looked conservative and left a single
	// successful forgery able to strand the sender exactly as before. MumbleFuzzcrypt found that too,
	// which is the second time this target has earned its keep.
	//
	// Half the window. A forgery can then displace the anchor by at most half of what the window
	// tolerates, leaving the other half for the genuine packets already on their way - and half the
	// window is also MAX_VIDEO_FRAGMENTS_PER_UNIT, so one whole unit may still arrive in any order.
	constexpr std::uint32_t VIDEO_MAX_SEQUENCE_JUMP = VIDEO_REPLAY_WINDOW / 2;

	// Consecutive authenticated packets beyond that jump after which the window re-anchors to them.
	//
	// A bound on its own would be a denial of service of its own making: if a sender ever did get more
	// than VIDEO_MAX_SEQUENCE_JUMP ahead - and at half a window that takes only a burst of loss, not an
	// outage - every later packet would be refused forever. So the receiver gives way, but only to
	// evidence it cannot be fed:
	// re-anchoring needs this many consecutive packets that authenticate, with no in-window packet
	// between them, and anything accepted normally resets the count. Holding the key is the only way to
	// produce one such packet; producing eight in a row without a genuine packet interrupting is not
	// something a forger at one-in-16.7-million gets to do.
	//
	// Only ever forward. A sequence *behind* the window stays refused however often it is offered, and
	// is refused without being decrypted, because a genuine sender's sequence only ever increases - so
	// re-anchoring backwards could only ever serve a replay of captured traffic.
	constexpr unsigned int VIDEO_RESYNC_PACKETS = 8;

	/**
	 * Encrypts and authenticates video datagrams on a sequence space of their own.
	 *
	 * Why this exists rather than reusing CryptStateOCB2 directly: that class derives its nonce from a
	 * one-byte rolling counter and accepts a late packet only if it is within 30 of the newest one. That
	 * is a reasonable fit for audio at fifty packets a second. Video is bursty and two orders of
	 * magnitude faster, and - measured against the real implementation - a burst of 31 packets delivered
	 * in reverse already starts failing to decrypt, while a burst of 64 loses over half. Sharing the
	 * counter with audio made it worse still: the tolerance is counted in packets, so adding video to
	 * the same sequence shrank audio's jitter tolerance from roughly 600 ms to under 100 ms.
	 *
	 * So video carries an explicit 32-bit sequence number in the clear and derives its nonce from it,
	 * and replay protection is a sliding window rather than a single counter. The AES-OCB primitive
	 * underneath is unchanged - this is the same ocb_encrypt/ocb_decrypt the audio path uses, driven
	 * with a different nonce discipline.
	 *
	 * Not thread safe: one instance per direction per peer, touched by one thread.
	 *
	 * Note for review: the construction here is conventional - explicit sequence, sequence-derived
	 * nonce, sliding replay window - but it is new cryptographic plumbing and should be reviewed as
	 * such before it carries anyone's camera.
	 */
	class VideoCryptState {
	public:
		enum class Result {
			Ok,
			/// Not addressed to the video channel.
			WrongChannel,
			/// Too short, or otherwise structurally invalid.
			Malformed,
			/// Sequence number already seen.
			Replay,
			/// Sequence number older than the replay window reaches.
			TooOld,
			/// Sequence number further ahead than VIDEO_MAX_SEQUENCE_JUMP allows, and not yet backed by
			/// the VIDEO_RESYNC_PACKETS consecutive authenticated packets that would re-anchor the
			/// window. Distinct from TooOld because it is the shape a forgery takes, and because an
			/// endpoint seeing a run of these is looking at either an attack or an outage it will
			/// recover from by itself.
			TooFarAhead,
			/// Authentication tag did not verify.
			AuthenticationFailed,
		};

		VideoCryptState() = default;

		/**
		 * @param key 16-byte AES key, shared by both directions.
		 * @param encryptNonceBase 12 bytes prefixed to the outgoing sequence to form the nonce.
		 * @param decryptNonceBase The peer's encryptNonceBase.
		 *
		 * The two bases must differ, or the two directions would reuse nonces under the same key, which
		 * is fatal for OCB. This is checked.
		 *
		 * Calling this again with the key and both bases already in force keeps the sequence counter and
		 * the replay window exactly as they are, rather than restarting them. For a fixed key the
		 * sequence is the whole of the nonce, so restarting it would spend nonces twice - see the
		 * implementation for why that is a break rather than a weakness. A different key does restart
		 * both, which is correct, its nonce space being untouched.
		 *
		 * A refused call changes nothing: the key already in force stays in force and isValid() still
		 * answers what it did before. That is deliberate, and not only so that a bad re-key leaves video
		 * working - invalidating the state on refusal would also discard the record of which key is
		 * installed, and the next call would then restart the sequence under a key it had already used.
		 */
		bool setKey(const std::string &key, const std::string &encryptNonceBase, const std::string &decryptNonceBase);

		/**
		 * Keys this state from the session key the audio path already negotiated, so that video needs no
		 * additional key exchange.
		 *
		 * The video key is derived rather than reused. Driving two different nonce constructions with one
		 * key would risk a video nonce colliding with an audio one - audio's is a 16-byte IV whose first
		 * byte rolls, video's is a 12-byte base plus a counter - and for OCB a nonce collision is a total
		 * break, not a weakness. Hashing with a distinct label per purpose keeps the two key streams
		 * independent, and gives each direction its own nonce base as setKey requires.
		 *
		 * Because the result is a pure function of the session key, calling this twice with the same
		 * session key reproduces the same key and the same nonce bases. setKey() is what makes that
		 * safe: it recognises the repeat and leaves the sequence counter alone. A caller must not
		 * reimplement the derivation and install it some other way.
		 *
		 * @param sessionKey The raw AES key from CryptSetup.
		 * @param isServer Which end this is, so the two ends pick opposite nonce bases.
		 */
		bool deriveFromSessionKey(const std::string &sessionKey, bool isServer);

		bool isValid() const { return m_valid; }

		/**
		 * Encrypts one plaintext into a complete datagram, ready for sendto().
		 *
		 * @returns false if the plaintext is too large, the state is not keyed, the sequence space is
		 *   exhausted, or the plaintext is one of the rare shapes OCB2's counter-cryptanalysis rejects.
		 *   Exhaustion is not silently ignored: reusing a nonce would be a real break, so the state
		 *   refuses to encrypt and the caller must rekey. The counter-cryptanalysis case is a refusal
		 *   here rather than the plaintext-altering workaround audio uses - see the implementation - so a
		 *   caller must treat false as "this fragment was not sent" and let retransmission deal with it,
		 *   never as "sent, probably".
		 */
		bool encrypt(std::span< const byte > plaintext, std::vector< byte > &datagram);

		/**
		 * Verifies and decrypts one received datagram.
		 *
		 * The replay window is only advanced once authentication succeeds, so a forged packet carrying a
		 * plausible sequence number cannot punch a hole that suppresses the genuine one.
		 */
		Result decrypt(std::span< const byte > datagram, std::vector< byte > &plaintext);

		/// Highest sequence number successfully received so far. 0 means none.
		std::uint32_t highestReceived() const { return m_highestReceived; }

		/// Sequence number the next encrypt() will use.
		std::uint32_t nextSequence() const { return m_nextSequence; }

		/// Counters, for the connection-quality display and for tests.
		std::uint64_t replayCount() const { return m_replays; }
		std::uint64_t tooOldCount() const { return m_tooOld; }
		std::uint64_t authFailureCount() const { return m_authFailures; }
		/// Packets refused for being further ahead than VIDEO_MAX_SEQUENCE_JUMP, and times the window
		/// re-anchored because VIDEO_RESYNC_PACKETS of them authenticated in a row. A non-zero forged
		/// count with no resyncs is what an attempted lockout looks like from the inside.
		std::uint64_t tooFarAheadCount() const { return m_tooFarAhead; }
		std::uint64_t resyncCount() const { return m_resyncs; }
		/// Frames OCB2's counter-cryptanalysis required altering by one bit before they could be sent.
		/// See the implementation. Reported because the alteration is undetectable by the receiver - the
		/// tag covers the altered plaintext - so without this number an artifact caused by it has no
		/// explanation available anywhere.
		std::uint64_t xexStarAlteredCount() const { return m_xexStarAltered; }

	protected:
		CryptStateOCB2 m_ocb;

		bool m_valid = false;

		unsigned char m_encryptNonceBase[12] = {};
		unsigned char m_decryptNonceBase[12] = {};

		std::uint32_t m_nextSequence    = VIDEO_FIRST_SEQUENCE;
		std::uint32_t m_highestReceived = 0;

		// Bit i means "the packet numbered (m_highestReceived - i) has been accepted". Bit 0 therefore
		// always corresponds to m_highestReceived itself.
		std::bitset< VIDEO_REPLAY_WINDOW > m_window;

		std::uint64_t m_replays      = 0;
		std::uint64_t m_tooOld       = 0;
		std::uint64_t m_authFailures = 0;
		std::uint64_t m_xexStarAltered = 0;
		std::uint64_t m_tooFarAhead  = 0;
		std::uint64_t m_resyncs      = 0;

		// Consecutive authenticated packets beyond the jump bound. Reset by anything accepted normally,
		// which is what stops a forger accumulating them one lucky packet at a time.
		unsigned int m_aheadStreak = 0;

		/// Checks a sequence against the replay window without modifying it. TooFarAhead means "do not
		/// trust this yet", not "reject it": decrypt() verifies such a packet before deciding.
		Result checkSequence(std::uint32_t sequence) const;

		/// Records a sequence as accepted, sliding the window forward if necessary.
		void commitSequence(std::uint32_t sequence);
	};

} // namespace Protocol
} // namespace Mumble

#endif // MUMBLE_VIDEOTRANSPORT_H_
