// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "VideoTransport.h"

#include "crypto/Digest.h"

#include <cstring>

namespace Mumble {
namespace Protocol {

	namespace {

		void writeBigEndian32(byte *dst, std::uint32_t value) {
			dst[0] = static_cast< byte >((value >> 24) & 0xFF);
			dst[1] = static_cast< byte >((value >> 16) & 0xFF);
			dst[2] = static_cast< byte >((value >> 8) & 0xFF);
			dst[3] = static_cast< byte >(value & 0xFF);
		}

		std::uint32_t readBigEndian32(const byte *src) {
			return (static_cast< std::uint32_t >(src[0]) << 24) | (static_cast< std::uint32_t >(src[1]) << 16)
				   | (static_cast< std::uint32_t >(src[2]) << 8) | static_cast< std::uint32_t >(src[3]);
		}

	} // namespace

	bool VideoCryptState::setKey(const std::string &key, const std::string &encryptNonceBase,
								 const std::string &decryptNonceBase) {
		// Everything is validated before anything is written, and in particular before m_valid is
		// touched. A refused call therefore leaves the state exactly as it was rather than invalidating
		// it, which matters for more than tidiness.
		//
		// When this cleared m_valid first, a refused call in the middle of a connection destroyed the
		// evidence the "unchanged" test below depends on: setKey(K, A, B), then a refused setKey(K, A,
		// A), then setKey(K, A, B) again saw m_valid false, concluded the parameters were new, and
		// restarted the sequence counter under key K - reintroducing exactly the nonce reuse the test
		// exists to prevent. Keeping a working key in force when a re-key is refused is also the better
		// behaviour in its own right: video keeps flowing instead of going silently dead.
		if (key.size() != AES_KEY_SIZE_BYTES || encryptNonceBase.size() != sizeof(m_encryptNonceBase)
			|| decryptNonceBase.size() != sizeof(m_decryptNonceBase)) {
			return false;
		}

		// Identical bases under one key would make the two directions produce identical nonces for the
		// same sequence number. For OCB, nonce reuse under the same key is a complete break, not a
		// degradation, so this is refused rather than merely discouraged.
		if (encryptNonceBase == decryptNonceBase) {
			return false;
		}

		// Whether this call re-installs the parameters already in force. Worked out before anything is
		// overwritten, because the answer decides whether the sequence counter may restart.
		const bool unchanged =
			m_valid && m_ocb.getRawKey() == key
			&& std::memcmp(m_encryptNonceBase, encryptNonceBase.data(), sizeof(m_encryptNonceBase)) == 0
			&& std::memcmp(m_decryptNonceBase, decryptNonceBase.data(), sizeof(m_decryptNonceBase)) == 0;

		if (!m_ocb.setRawKey(key)) {
			// Unreachable: setRawKey only rejects a wrong length, checked above. Kept because an
			// unchecked return here would be the kind of omission that stops being unreachable later.
			m_valid = false;

			return false;
		}

		std::memcpy(m_encryptNonceBase, encryptNonceBase.data(), sizeof(m_encryptNonceBase));
		std::memcpy(m_decryptNonceBase, decryptNonceBase.data(), sizeof(m_decryptNonceBase));

		// Re-installing the same parameters must not restart the sequence, and this is the whole reason
		// the comparison above exists.
		//
		// The nonce is the encrypt base followed by the sequence number, so for a fixed key the sequence
		// is the entirety of the nonce's variability. Restarting it would encrypt new frames under nonces
		// this state has already spent, and OCB does not merely weaken under nonce reuse: two ciphertexts
		// sharing a nonce leak the XOR of their plaintexts, and the authentication tag stops being a
		// forgery barrier at all. Video frames share header structure, so that XOR is close to plaintext.
		//
		// deriveFromSessionKey() makes this reachable rather than theoretical, because its output is a
		// pure function of the session key: anything that delivers the same session key twice - a peer
		// re-sending CryptSetup, through a bug or otherwise - lands here with every parameter identical.
		// Audio does not have the problem, since there the peer supplies fresh random IVs and a repeat is
		// a new nonce; video derives its base, so the sequence is the only thing standing between a
		// redundant re-key and a total break.
		//
		// Keeping the counter where it is makes a redundant re-key harmless. A genuinely different key
		// still gets a fresh counter, which is correct: that key's nonce space is untouched.
		//
		// The replay window is held for the same reason. Clearing it would re-admit every datagram the
		// peer has already sent under this key, which is a replay window that has stopped preventing
		// replay.
		if (!unchanged) {
			m_nextSequence    = VIDEO_FIRST_SEQUENCE;
			m_highestReceived = 0;
			m_window.reset();

			m_replays      = 0;
			m_tooOld       = 0;
			m_authFailures = 0;
			m_xexStarAltered = 0;
			m_tooFarAhead  = 0;
			m_resyncs      = 0;
			m_aheadStreak  = 0;
		}

		m_valid = true;

		return true;
	}

	bool VideoCryptState::deriveFromSessionKey(const std::string &sessionKey, bool isServer) {
		if (sessionKey.size() != AES_KEY_SIZE_BYTES) {
			return false;
		}

		const auto derive = [&sessionKey](const char *label, std::size_t bytes) {
			std::vector< std::uint8_t > input(sessionKey.begin(), sessionKey.end());
			input.insert(input.end(), label, label + std::strlen(label));

			const std::vector< std::uint8_t > digest = Crypto::Digest::hash(input, Crypto::DigestAlgorithm::Sha256);

			return std::string(reinterpret_cast< const char * >(digest.data()), bytes);
		};

		// Three independent labels: one for the key, one for each direction's nonce base. The server
		// takes "s2c" as its outgoing base and the client takes "c2s", so the two ends agree without
		// exchanging anything and never share a base.
		const std::string videoKey       = derive("mumble-video-key", AES_KEY_SIZE_BYTES);
		const std::string serverToClient = derive("mumble-video-nonce-s2c", sizeof(m_encryptNonceBase));
		const std::string clientToServer = derive("mumble-video-nonce-c2s", sizeof(m_encryptNonceBase));

		if (isServer) {
			return setKey(videoKey, serverToClient, clientToServer);
		}

		return setKey(videoKey, clientToServer, serverToClient);
	}

	bool VideoCryptState::encrypt(std::span< const byte > plaintext, std::vector< byte > &datagram) {
		if (!m_valid || plaintext.empty() || plaintext.size() > MAX_VIDEO_PLAINTEXT_SIZE) {
			return false;
		}

		// The sequence is the nonce. Wrapping it would repeat a nonce under the same key, so the state
		// stops rather than wrapping and the connection must rekey. At a sustained 30k packets per
		// second this is roughly forty hours of continuous streaming.
		if (m_nextSequence == 0xFFFFFFFFu) {
			return false;
		}

		const std::uint32_t sequence = m_nextSequence;

		unsigned char nonce[AES_BLOCK_SIZE];
		std::memcpy(nonce, m_encryptNonceBase, sizeof(m_encryptNonceBase));
		writeBigEndian32(reinterpret_cast< byte * >(nonce) + sizeof(m_encryptNonceBase), sequence);

		datagram.resize(VIDEO_HEADER_SIZE + plaintext.size());

		datagram[0] = MEDIA_CHANNEL_VIDEO;
		writeBigEndian32(datagram.data() + VIDEO_SEQUENCE_OFFSET, sequence);

		unsigned char tag[AES_BLOCK_SIZE];

		// The counter-cryptanalysis case is delivered, like audio, but counted rather than left silent.
		//
		// ocb_encrypt carries the counter-cryptanalysis from eprint 2019/311. When the plaintext's
		// second-to-last block is fifteen zero bytes, the construction is attackable, and the default
		// behaviour is to flip one bit of the plaintext and fold the same flip into the checksum. The tag
		// then verifies against the altered plaintext, so the receiver cannot tell: it gets a packet that
		// authenticates perfectly and differs from what was sent.
		//
		// For audio that was a considered trade - upstream notes digital silence produces such packets in
		// quantity, and one bit in an Opus frame is inaudible. The first attempt here was to refuse such
		// a frame instead, on the reasoning that a silently altered compressed bitstream is a worse
		// failure than a dropped fragment. The tests said otherwise, and they were right: a payload of
		// 64 zero bytes trips the condition, so TestVideoTransport refused to send three of its own
		// fixtures. A payload with a zero run at the wrong offset is not exotic - a flat region of a
		// screen can produce one - and refusing it is deterministic for that payload, so a region could
		// simply stop updating, repeatedly, for as long as it looked like that. One altered bit that
		// arrives beats a fragment that never does.
		//
		// So it is sent, as audio sends it, and counted. The counter is the part that was missing: the
		// alteration was previously invisible at every level, and a receiver has no way to detect it
		// because the tag is computed over the altered plaintext. One bit in a compressed frame is
		// something the existing corrupt-frame detection and keyframe requests already recover from; an
		// unexplained artifact with no number anywhere is not.
		//
		// Costed deliberately: the first call asks for the refusal so the condition can be observed, and
		// only the rare failing case pays for a second pass. Re-encrypting is safe because ocb_encrypt
		// is a pure function of its inputs and the sequence is not consumed until this succeeds.
		//
		// Found by MumbleFuzzcrypt, whose round-trip invariant it broke.
		if (!m_ocb.ocb_encrypt(plaintext.data(), datagram.data() + VIDEO_HEADER_SIZE,
							   static_cast< unsigned int >(plaintext.size()), nonce, tag, false)) {
			m_xexStarAltered++;

			if (!m_ocb.ocb_encrypt(plaintext.data(), datagram.data() + VIDEO_HEADER_SIZE,
								   static_cast< unsigned int >(plaintext.size()), nonce, tag, true)) {
				datagram.clear();

				return false;
			}
		}

		std::memcpy(datagram.data() + VIDEO_TAG_OFFSET, tag, VIDEO_TAG_SIZE);

		m_nextSequence++;

		return true;
	}

	VideoCryptState::Result VideoCryptState::checkSequence(std::uint32_t sequence) const {
		if (sequence == 0) {
			return Result::Malformed;
		}

		if (sequence > m_highestReceived) {
			// Ahead of everything accepted, which is the normal case - but only believable up to a
			// point. See VIDEO_MAX_SEQUENCE_JUMP: the anchor below is what a forged packet carrying a
			// huge sequence used to move, locking the genuine sender out permanently.
			//
			// The bound applies from a cold start too. A sender begins at VIDEO_FIRST_SEQUENCE, so the
			// first packet to arrive is near it; without this, a single forgery landing before any
			// genuine packet could set the anchor anywhere.
			if (sequence - m_highestReceived > VIDEO_MAX_SEQUENCE_JUMP) {
				return Result::TooFarAhead;
			}

			return Result::Ok;
		}

		if (m_highestReceived == 0) {
			// Nothing accepted yet, and the sequence is not above zero - which only sequence 0 is, and
			// that is rejected above as malformed.
			return Result::Ok;
		}

		const std::uint32_t behind = m_highestReceived - sequence;

		if (behind >= VIDEO_REPLAY_WINDOW) {
			return Result::TooOld;
		}

		if (m_window.test(behind)) {
			return Result::Replay;
		}

		return Result::Ok;
	}

	void VideoCryptState::commitSequence(std::uint32_t sequence) {
		if (sequence > m_highestReceived) {
			const std::uint32_t advance = sequence - m_highestReceived;

			if (m_highestReceived == 0 || advance >= VIDEO_REPLAY_WINDOW) {
				// A jump larger than the window leaves nothing worth keeping.
				m_window.reset();
			} else {
				m_window <<= advance;
			}

			m_highestReceived = sequence;
			m_window.set(0);

			return;
		}

		const std::uint32_t behind = m_highestReceived - sequence;

		if (behind < VIDEO_REPLAY_WINDOW) {
			m_window.set(behind);
		}
	}

	VideoCryptState::Result VideoCryptState::decrypt(std::span< const byte > datagram, std::vector< byte > &plaintext) {
		if (!m_valid) {
			return Result::Malformed;
		}

		if (datagram.size() <= VIDEO_HEADER_SIZE || datagram.size() > MAX_VIDEO_DATAGRAM_SIZE) {
			return Result::Malformed;
		}

		if (datagram[0] != MEDIA_CHANNEL_VIDEO) {
			return Result::WrongChannel;
		}

		const std::uint32_t sequence = readBigEndian32(datagram.data() + VIDEO_SEQUENCE_OFFSET);

		// Checked before doing the work of decrypting, so a flood of replayed packets is cheap to
		// refuse, but committed only afterwards so a forgery cannot mark a sequence as seen.
		const Result sequenceCheck = checkSequence(sequence);

		// Replay and TooOld are refused here, without decrypting. Both are cheap to refuse and neither
		// can ever become acceptable: a genuine sender's sequence only increases, so the only thing that
		// could be served by reconsidering an old sequence is a replay of captured traffic.
		if (sequenceCheck == Result::Replay || sequenceCheck == Result::TooOld
			|| sequenceCheck == Result::Malformed) {
			if (sequenceCheck == Result::Replay) {
				m_replays++;
			} else if (sequenceCheck == Result::TooOld) {
				m_tooOld++;
			}

			return sequenceCheck;
		}

		// TooFarAhead is not refused here, because it is the one rejection a genuine sender can
		// legitimately provoke - by getting far ahead during an outage - and the only way to tell that
		// apart from a forgery is to check the tag. So the packet is decrypted below and judged
		// afterwards: authentic ones count towards a re-anchor, forged ones are thrown away having cost
		// the attacker a packet and us one AES pass, which is exactly what any forgery attempt already
		// costs.

		unsigned char nonce[AES_BLOCK_SIZE];
		std::memcpy(nonce, m_decryptNonceBase, sizeof(m_decryptNonceBase));
		writeBigEndian32(reinterpret_cast< byte * >(nonce) + sizeof(m_decryptNonceBase), sequence);

		const std::size_t payloadSize = datagram.size() - VIDEO_HEADER_SIZE;

		plaintext.resize(payloadSize);

		unsigned char tag[AES_BLOCK_SIZE];

		if (!m_ocb.ocb_decrypt(datagram.data() + VIDEO_HEADER_SIZE, plaintext.data(),
							   static_cast< unsigned int >(payloadSize), nonce, tag)) {
			plaintext.clear();
			m_authFailures++;

			return Result::AuthenticationFailed;
		}

		if (std::memcmp(tag, datagram.data() + VIDEO_TAG_OFFSET, VIDEO_TAG_SIZE) != 0) {
			plaintext.clear();
			m_authFailures++;

			return Result::AuthenticationFailed;
		}

		// Authentic. If it was further ahead than the jump bound allows, it still is not accepted on its
		// own - it has to be one of a run of them, because a single authentic-looking packet at an
		// absurd sequence is what a successful forgery looks like, and accepting it is what used to end
		// the connection's video for good.
		if (sequenceCheck == Result::TooFarAhead) {
			m_tooFarAhead++;
			m_aheadStreak++;

			if (m_aheadStreak < VIDEO_RESYNC_PACKETS) {
				plaintext.clear();

				return Result::TooFarAhead;
			}

			// A run this long, with nothing in-window arriving in between, is a sender that really has
			// moved on. Re-anchor to it: the window is cleared because nothing it held is relevant to
			// the new position.
			m_resyncs++;
			m_aheadStreak     = 0;
			m_highestReceived = 0;
			m_window.reset();
		}

		// Anything accepted normally is evidence the anchor is right, so the run has to start over.
		m_aheadStreak = 0;

		commitSequence(sequence);

		return Result::Ok;
	}

} // namespace Protocol
} // namespace Mumble
