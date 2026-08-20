// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "VideoStreamDispatcher.h"

#include "Mumble.pb.h"

void VideoStreamDispatcher::setStreamCodec(unsigned int senderSession, unsigned int streamID, int codec) {
	StreamInfo &info    = m_streams[streamKey(senderSession, streamID)];
	info.senderSession = senderSession;
	info.codec         = codec;
}

void VideoStreamDispatcher::removeSender(unsigned int senderSession, unsigned int streamID) {
	m_streams.erase(streamKey(senderSession, streamID));
}

void VideoStreamDispatcher::removeSender(unsigned int senderSession) {
	for (auto it = m_streams.begin(); it != m_streams.end();) {
		if (it->second.senderSession == senderSession) {
			it = m_streams.erase(it);
		} else {
			++it;
		}
	}
}

void VideoStreamDispatcher::clear() {
	m_streams.clear();
}

void VideoStreamDispatcher::onVideoUnitReceived(unsigned int senderSession, unsigned int streamID,
												quint64 /* frameNumber */, bool /* isKeyframe */,
												unsigned int x, unsigned int y, const QByteArray &payload) {
	const auto it = m_streams.find(streamKey(senderSession, streamID));

	if (it == m_streams.end() || it->second.codec != MumbleProto::VideoState_Codec_OpusAudio) {
		return;
	}

	emit opusUnitReceived(senderSession, streamID, payload);
}
