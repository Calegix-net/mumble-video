// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_VIDEOBROADCASTER_H_
#define MUMBLE_MUMBLE_VIDEOBROADCASTER_H_

#include "VP8Codec.h"
#include "VideoEncoder.h"
#include "VideoFragmentation.h"

#include <QtCore/QObject>
#include <QtCore/QString>

#include <cstdint>
#include <memory>

class VideoSource;

/**
 * Turns a capture source into a stream of encoded units ready to be sent.
 *
 * This is the send half of the pipeline, and it exists as its own class rather than living in
 * MainWindow so that the encode-and-emit behaviour can be tested against a synthetic source, with no
 * camera, no window and no network.
 *
 * It does not know how to send anything. It emits units and lets its owner decide what to do with them,
 * which keeps the capture and encode path independent of the transport underneath it.
 */
class VideoBroadcaster : public QObject {
	Q_OBJECT

public:
	explicit VideoBroadcaster(QObject *parent = nullptr);
	~VideoBroadcaster() override;

	/**
	 * Starts capturing and encoding from the given source, which this object takes ownership of.
	 *
	 * Each start allocates a new stream id, so a receiver treats it as a new picture rather than
	 * continuing to paint into the surface from the previous one, whose dimensions may differ.
	 *
	 * @returns Whether the source started.
	 */
	bool start(std::unique_ptr< VideoSource > source);

	void stop();

	bool isActive() const { return m_source != nullptr; }

	std::uint32_t streamID() const { return m_streamID; }

	/**
	 * Seeds the stream id this broadcaster will use for its next (first) start(), instead of the default
	 * of 0.
	 *
	 * A session that runs more than one broadcaster at once - a camera and a screen share, or a screen
	 * share's video alongside its audio - must not let two of them allocate the same id independently:
	 * VideoRouter and VideoFragmentation both key on (sender, stream), which is the same sender for all
	 * of them, and there is nothing else in the wire format to tell two same-numbered streams apart. The
	 * owner (MainWindow) is expected to hand out a distinct base per broadcaster instance before ever
	 * starting it. Calling this after the first start() has no defined effect and is a caller error.
	 */
	/// Largest frame the encoder is fed, matching VideoGrid's surface bounds on the receiving side.
	/// One constant on each side rather than a shared header, but they must agree: the receiver
	/// refuses what exceeds its bound rather than scaling it.
	static constexpr int MAX_ENCODED_WIDTH  = 3840;
	static constexpr int MAX_ENCODED_HEIGHT = 2160;

	void setNextStreamID(std::uint32_t id) {
		m_streamID = id;
		// Remembered so start() does not ALSO auto-increment: callers that allocate ids from a shared
		// counter seed before every start, and auto-incrementing on top of that made the second share's
		// id collide with the next allocation.
		m_streamIDSeeded = true;
	}

	/// Description of what is being captured, for the UI.
	QString describe() const;

	/// Forces the next frame to be sent in full, in response to a receiver asking for a keyframe or a
	/// new subscriber arriving.
	void requestKeyframe();

	TiledImageEncoder &encoder() { return m_encoder; }

	/// Which codec to encode with. 0 = VP8, 1 = TiledImage, matching Settings::videoCodec.
	void setCodec(int codec);
	int codec() const { return m_codec; }

	/// Applies the capture and quality settings. Called before start() so the first frame already uses
	/// them; VP8 in particular cannot change resolution mid-stream.
	void configure(int codec, unsigned int bitrateKbps, unsigned int framerate, int tileQuality, int tileSize);

signals:
	/// One encoded unit, ready to fragment and send.
	void unitReady(const Mumble::Protocol::VideoUnitHeader &header, const QByteArray &payload);

	/// The frame just captured, before encoding.
	///
	/// Self-view deliberately shows the raw capture rather than the encoded-and-decoded result: the point
	/// is to confirm framing and that the camera is live, and putting your own encoder and decoder in that
	/// path would cost CPU to make your own picture look worse.
	void previewFrame(const QImage &frame);

	/// Capture started or stopped, so the UI can update the toggle.
	void activeChanged(bool active);

	/// Capture failed after starting: device unplugged, permission revoked, backend error.
	void failed(const QString &reason);

protected slots:
	void onFrameReady(const QImage &frame, std::uint64_t captureTimestampUsec);

protected:
	std::unique_ptr< VideoSource > m_source;
	TiledImageEncoder m_encoder;
	VP8Encoder m_vp8;

	// 0 = VP8, 1 = TiledImage. Neither is a better default in general: VP8 is right for a camera and
	// ruinously wrong for a still screen, and tiled JPEG is the reverse.
	int m_codec = 0;

	// Not reset between streams: a receiver keys reassembly on (sender, stream, frame, unit), and reusing
	// a frame number that a stale in-flight fragment also carries would let the two merge.
	std::uint64_t m_frameNumber = 0;

	// 0 is a valid stream id, so the first stream uses it and each subsequent start increments.
	std::uint32_t m_streamID = 0;
	bool m_everStarted       = false;
	bool m_streamIDSeeded    = false;

	bool m_forceKeyframe = true;
};

#endif // MUMBLE_MUMBLE_VIDEOBROADCASTER_H_
