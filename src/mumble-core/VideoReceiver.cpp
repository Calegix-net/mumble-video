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
		const std::size_t size = 1 + nack.ByteSizeLong();

		// Must fit a single video datagram once the crypt header is added, the same as any other video
		// packet. collectNacks() already batches so a message stays within this, but a message built any
		// other way must not be put on the wire oversized to be IP-fragmented or dropped; the caller
		// treats an empty return as "skip this one".
		if (size > MAX_VIDEO_PLAINTEXT_SIZE) {
			return {};
		}

		std::vector< byte > packet(size);
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

			const auto addUnit = [&](Batch &b) {
				MumbleUDP::VideoNack::Unit *unit = b.message.add_units();
				unit->set_frame_number(request.frameNumber);
				unit->set_unit_id(request.unitID);
				unit->set_missing_fragments(request.missingFragments);
				for (std::uint32_t index : request.missingIndices) {
					unit->add_missing_indices(index);
				}
				b.requests.push_back(&request);
			};
			const auto newBatch = [&]() -> Batch & {
				forStream.emplace_back();
				forStream.back().message.set_sender_session(request.senderSession);
				forStream.back().message.set_stream_id(request.streamID);
				return forStream.back();
			};

			if (forStream.empty()) {
				addUnit(newBatch());
				continue;
			}

			Batch &current = forStream.back();
			addUnit(current);

			// Start a new batch if that unit took this one over the unit cap or - the part the count cap
			// alone misses - past what one datagram holds. A unit of more than 64 fragments carries a
			// missing_indices list rather than the 64-bit mask, and a large keyframe lost in bulk can make
			// that list long enough that even a handful of such units would not fit a single VideoNack
			// datagram; left unsplit, encodeVideoNackPacket() would refuse the whole message and none of
			// those units would be asked for. One unit always fits alone (its indices are fragment numbers
			// below MAX_VIDEO_FRAGMENTS_PER_UNIT), so moving the last one out can never loop.
			if ((current.message.units_size() > MAX_VIDEO_NACK_UNITS
				 || 1 + current.message.ByteSizeLong() > MAX_VIDEO_PLAINTEXT_SIZE)
				&& current.message.units_size() > 1) {
				current.message.mutable_units()->RemoveLast();
				current.requests.pop_back();
				addUnit(newBatch());
			}
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
