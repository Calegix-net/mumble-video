// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "VideoReceiver.h"

#include <map>
#include <utility>

namespace Mumble {
namespace Protocol {

	std::vector< byte > encodeVideoNackPacket(const MumbleUDP::VideoNack &nack) {
		std::vector< byte > packet(1 + nack.ByteSizeLong());
		packet[0] = static_cast< byte >(UDPMessageType::VideoNack);
		if (!nack.SerializeToArray(packet.data() + 1, static_cast< int >(packet.size() - 1))) {
			return {};
		}

		return packet;
	}

	void VideoReceiver::receive(VideoCryptState &crypt, std::span< const byte > datagram, std::uint64_t nowUsec,
								VideoReceiveEvents &events) {
		if (!crypt.isValid()) {
			return;
		}

		std::vector< byte > plaintext;

		if (crypt.decrypt(datagram, plaintext) != VideoCryptState::Result::Ok) {
			return;
		}

		if (plaintext.size() >= 2 && plaintext[0] == static_cast< byte >(UDPMessageType::VideoNack)) {
			// A viewer asking for part of one of our streams again, forwarded by the server.
			events.nacksForUs.emplace_back(plaintext.begin() + 1, plaintext.end());
			return;
		}

		if (plaintext.size() < 2 || plaintext[0] != static_cast< byte >(UDPMessageType::Video)) {
			return;
		}

		// The reassembler keys on an authenticated sender rather than on whatever the packet claims. On this
		// side that identity is the one the server stamped: the server is the only party that can attribute a
		// relayed packet, and it is already trusted to route them at all.
		m_peek.Clear();

		if (!m_peek.ParseFromArray(plaintext.data() + 1, static_cast< int >(plaintext.size() - 1))) {
			return;
		}

		const std::uint32_t senderSession = m_peek.sender_session();

		if (senderSession == 0) {
			// Unstamped, so unattributable. A conforming server always stamps.
			return;
		}

		if (nowUsec - m_lastNackCollectUsec >= NACK_COLLECT_INTERVAL_USEC) {
			m_lastNackCollectUsec = nowUsec;
			collectNacks(nowUsec, events);
		}

		VideoUnit unit;
		if (m_reassembler.processPacket(plaintext, senderSession, nowUsec, unit) == VideoReassemblyResult::Complete) {
			events.units.push_back(std::move(unit));
		}
	}

	void VideoReceiver::collectNacks(std::uint64_t nowUsec, VideoReceiveEvents &events) {
		events.collectedNacks = true;

		std::vector< VideoNackRequest > requests;
		m_reassembler.collectNacks(nowUsec, requests);

		if (requests.empty()) {
			return;
		}
		events.unitsRequested += requests.size();

		if (nowUsec - m_nackWindowStartUsec >= 1000 * 1000) {
			m_nackWindowStartUsec  = nowUsec;
			m_nackMessagesInWindow = 0;
		}

		// Messages per stream, each carrying up to MAX_VIDEO_NACK_UNITS of it. More than one where a
		// stream has more missing units than that: the units beyond the first batch used to be dropped on
		// the floor while still being counted as asked for, so the same first 32 won every time and the
		// rest were never requested at all.
		struct Batch {
			MumbleUDP::VideoNack message;
			std::vector< const VideoNackRequest * > requests;
		};

		std::map< std::pair< std::uint32_t, std::uint32_t >, std::vector< Batch > > batches;

		for (const VideoNackRequest &request : requests) {
			std::vector< Batch > &forStream = batches[{ request.senderSession, request.streamID }];

			if (forStream.empty() || forStream.back().message.units_size() >= MAX_VIDEO_NACK_UNITS) {
				forStream.emplace_back();
				forStream.back().message.set_sender_session(request.senderSession);
				forStream.back().message.set_stream_id(request.streamID);
			}

			Batch &batch                     = forStream.back();
			MumbleUDP::VideoNack::Unit *unit = batch.message.add_units();
			unit->set_frame_number(request.frameNumber);
			unit->set_unit_id(request.unitID);
			unit->set_missing_fragments(request.missingFragments);
			for (std::uint32_t index : request.missingIndices) {
				unit->add_missing_indices(index);
			}

			batch.requests.push_back(&request);
		}

		for (const auto &entry : batches) {
			for (const Batch &batch : entry.second) {
				if (m_nackMessagesInWindow >= MAX_NACK_MESSAGES_PER_SECOND) {
					// Out of budget for this second. Nothing is recorded as asked for, so these units are
					// collected again on the next tick rather than silently losing an attempt here.
					return;
				}

				std::vector< byte > packet = encodeVideoNackPacket(batch.message);

				if (packet.empty()) {
					// Would not encode or would not fit a datagram. Again, no attempt is spent.
					continue;
				}

				++m_nackMessagesInWindow;
				events.packetsToSend.push_back(std::move(packet));

				// Only now is the attempt against each of these units real.
				for (const VideoNackRequest *request : batch.requests) {
					m_reassembler.noteNackSent(*request, nowUsec);
				}
			}
		}
	}

} // namespace Protocol
} // namespace Mumble
