// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "MediaDemux.h"

namespace Mumble {
namespace Protocol {

	// The smallest datagram worth looking at: OCB2's four header bytes and one more, the same floor stock
	// Mumble applies. With a channel byte that leaves an audio payload of at least the four header bytes.
	static constexpr std::size_t MIN_MEDIA_DATAGRAM_SIZE = 5;

	MediaDatagram MediaDemux::classify(std::span< const byte > datagram) const {
		MediaDatagram result;
		result.datagram = datagram;

		if (datagram.size() < MIN_MEDIA_DATAGRAM_SIZE) {
			return result;
		}

		if (m_framing == Framing::Plain) {
			if (datagram.size() > MAX_AUDIO_PAYLOAD_SIZE) {
				return result;
			}

			result.channel = MediaChannel::Audio;
			result.payload = datagram;

			return result;
		}

		switch (datagram[0]) {
			case MEDIA_CHANNEL_AUDIO:
				// Longer than any audio plaintext the decoder's buffer can take. See MAX_AUDIO_PAYLOAD_SIZE.
				if (datagram.size() - 1 > MAX_AUDIO_PAYLOAD_SIZE) {
					break;
				}

				result.channel = MediaChannel::Audio;
				result.payload = datagram.subspan(1);
				break;
			case MEDIA_CHANNEL_VIDEO:
				result.channel = MediaChannel::Video;
				result.payload = datagram.subspan(1);
				break;
			default:
				break;
		}

		return result;
	}

} // namespace Protocol
} // namespace Mumble
