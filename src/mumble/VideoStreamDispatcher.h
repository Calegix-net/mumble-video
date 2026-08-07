// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_VIDEOSTREAMDISPATCHER_H_
#define MUMBLE_MUMBLE_VIDEOSTREAMDISPATCHER_H_

#include <QtCore/QByteArray>
#include <QtCore/QObject>

#include <cstdint>
#include <unordered_map>

/**
 * Routes incoming video-transport units to the right consumer, based on the codec their sender announced.
 *
 * ServerHandler::videoUnitReceived carries every unit from every subscribed stream through one signal,
 * regardless of what the stream actually is. Historically the only consumer was VideoGrid, which quietly
 * drops anything it cannot decode as a picture - harmless for a codec it does not know, but it also means
 * nothing plays those units either. A screen share's accompanying audio rides this same transport, tagged
 * with the OpusAudio codec (see Mumble.proto's VideoState::Codec) specifically so it can reuse the video
 * pipeline's crypto, fragmentation and subscription routing rather than needing any of its own - but that
 * only works if something downstream actually looks at the codec and sends OpusAudio units somewhere that
 * plays them. This class is that something.
 *
 * It listens to the same ServerHandler::videoUnitReceived signal VideoGrid does, in parallel - neither
 * class knows about the other, and each simply ignores whatever it does not track.
 */
class VideoStreamDispatcher : public QObject {
	Q_OBJECT

public:
	explicit VideoStreamDispatcher(QObject *parent = nullptr) : QObject(parent) {}

	/**
	 * Records the codec a sender's stream carries, from its VideoState announcement. Mirrors
	 * VideoGrid::setStreamCodec, called alongside it so this class and the grid agree on what every
	 * stream is.
	 *
	 * @param codec A MumbleProto::VideoState::Codec value.
	 */
	void setStreamCodec(unsigned int senderSession, unsigned int streamID, int codec);

	/// Drops one of a sender's streams, when that stream ends.
	void removeSender(unsigned int senderSession, unsigned int streamID);

	/// Drops everything belonging to a sender, on disconnect.
	void removeSender(unsigned int senderSession);

	/// Drops everything, on disconnect from the server.
	void clear();

public slots:
	/// Connected to the same signal VideoGrid::onVideoUnitReceived is, and filters it the same way: only
	/// a stream announced with the codec this class cares about produces anything.
	void onVideoUnitReceived(unsigned int senderSession, unsigned int streamID, unsigned int x, unsigned int y,
							 const QByteArray &payload);

signals:
	/// One Opus packet belonging to a sender's screen-share audio stream. x/y/width/height from the
	/// underlying video transport are meaningless for audio and are not carried here.
	void opusUnitReceived(unsigned int senderSession, unsigned int streamID, const QByteArray &opusPacket);

protected:
	static std::uint64_t streamKey(unsigned int senderSession, unsigned int streamID) {
		return (static_cast< std::uint64_t >(senderSession) << 32) | streamID;
	}

	// MumbleProto::VideoState::Codec per (sender, stream). Only entries whose codec this class actually
	// acts on need be looked up on the hot path, but every announced stream is recorded so removeSender
	// can find them by sender alone.
	struct StreamInfo {
		unsigned int senderSession = 0;
		int codec                  = 0;
	};

	std::unordered_map< std::uint64_t, StreamInfo > m_streams;
};

#endif // MUMBLE_MUMBLE_VIDEOSTREAMDISPATCHER_H_
