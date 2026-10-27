// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_VIDEOBROADCASTER_H_
#define MUMBLE_MUMBLE_VIDEOBROADCASTER_H_

#include "VideoEncoder.h"
#include "VideoFrameDump.h"
#include "VideoFragmentation.h"
#include "VpxCodec.h"
#ifdef USE_H264
#	include "H264Codec.h"
#endif

#include <QtCore/QElapsedTimer>
#include <QtCore/QObject>
#include <QtCore/QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

class QThread;
class VideoSource;

/**
 * The expensive half of VideoBroadcaster: scaling a captured frame, noticing that a screen has stopped
 * changing, and encoding. Owns the encoders and hands out frame numbers.
 *
 * Kept apart from VideoBroadcaster so that it can run on a thread of its own (see
 * VideoBroadcaster::setThreadedEncoding()). Every method is called from one thread at a time - whichever
 * one that is - so it needs no locking of its own.
 */
class VideoEncodeCore {
public:
	struct Job {
		QImage frame;
		/// The size to encode at: frame's size, scaled to fit the codec's limit. Worked out by the caller,
		/// which needs it before encoding to tell a resize, while the scaling itself happens here.
		QSize targetSize;
		std::uint32_t streamID             = 0;
		std::uint64_t captureTimestampUsec = 0;
		bool forceKeyframe                 = false;
	};

	struct Result {
		std::vector< EncodedVideoUnit > units;
		/// Whether anything was encoded: false when a still screen was skipped.
		bool encoded                       = false;
		bool keyframeRequested             = false;
		std::uint64_t captureTimestampUsec = 0;
		std::uint64_t encodeUsec           = 0;
	};

	void configure(int codec, unsigned int bitrateKbps, unsigned int framerate, int tileQuality, int tileSize);
	void setCodec(int codec);
	/// Starts over at a keyframe. @p reopenHardware also closes the hardware H.264 encoder, which is slow
	/// (a hundred milliseconds or more on some drivers) and only needed when a stream starts.
	void reset(bool reopenHardware);
	Result process(const Job &job);

	int codec() const { return m_codec; }
	bool isScreenCodec() const;

private:
	TiledImageEncoder m_encoder;
	VpxEncoder m_vpx;
#ifdef USE_H264
	H264Encoder m_h264;
#endif

	int m_codec              = 0;
	unsigned int m_framerate = 30;

	// Not reset between streams: a receiver keys reassembly on (sender, stream, frame, unit), and reusing
	// a frame number that a stale in-flight fragment also carries would let the two merge. Handed out
	// here rather than by the caller because a skipped still frame must not use one up: a gap in frame
	// numbers is what tells a viewer a frame was lost, and it would wait for one that never existed.
	std::uint64_t m_frameNumber = 0;

	QImage m_lastEncoded;
	// Consecutive frames identical to the one before, for the screen codecs' still-screen handling.
	unsigned int m_unchangedFrames      = 0;
	std::uint64_t m_lastEncodeTimestamp = 0;

	/// Where frames handed to the encoder are saved for a picture-quality comparison, or empty - which
	/// is every build that is not being benchmarked. See VideoFrameDump.
	QString m_dumpDirectory = VideoFrameDump::senderDirectory();
};

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
	void setStreamIDAllocator(std::function< std::uint32_t() > allocator) { m_allocateStreamID = std::move(allocator); }

	bool isActive() const { return m_source != nullptr; }

	std::uint32_t streamID() const { return m_streamID; }

	/// Largest frame the encoder is fed, matching VideoGrid's surface bounds on the receiving side.
	/// One constant on each side rather than a shared header, but they must agree: the receiver
	/// refuses what exceeds its bound rather than scaling it.
	static constexpr int MAX_ENCODED_WIDTH  = 3840;
	static constexpr int MAX_ENCODED_HEIGHT = 2160;
	/// A VP9 screen share is scaled to fit 2560x1440. At the bitrates a home uplink carries, more pixels
	/// do not make text sharper - they make every frame softer - and a 4K keyframe could exceed the
	/// transport's largest unit (VideoFragmenter::maxUnitSize(), about 275 KB).
	static constexpr int MAX_VP9_WIDTH  = 2560;
	static constexpr int MAX_VP9_HEIGHT = 1440;

	/// Smallest frame a hardware H.264 encoder will accept.
	///
	/// NVENC refuses to open below about 145x49 and the refusal is final: H264Encoder marks that size
	/// failed and returns no units at all from then on, so the stream is announced, carries nothing, and
	/// every viewer sits asking for a keyframe until their tile gives up. Nothing could reach those sizes
	/// while a share was always a whole display or a whole window; sharing a region can be any size at
	/// all, including a 138x46 sliver of a toolbar.
	///
	/// Above the hardware minimum with room to spare, because the figure varies between GPU generations
	/// and the encoder rounds odd dimensions down before opening.
	static constexpr int MIN_H264_WIDTH  = 160;
	static constexpr int MIN_H264_HEIGHT = 64;

	/// libvpx has no comparable floor, but H264Encoder refuses anything under 16 pixels outright and a
	/// frame that small is not worth sending. Applied to every codec so the floor exists either way.
	static constexpr int MIN_ENCODED_WIDTH  = 32;
	static constexpr int MIN_ENCODED_HEIGHT = 32;

	/// The size a frame is encoded at: clamped down to what receivers accept, then raised to what the
	/// codec's encoder will open at.
	///
	/// Takes the codec rather than reading the member, because which codecs exist depends on how the
	/// build was configured - setCodec() quietly substitutes VP8 for H.264 in a build without USE_H264 -
	/// and the arithmetic here is worth checking for a codec this particular binary cannot encode with.
	static QSize encodeSizeFor(const QSize &source, int codec);

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

	/// Which codec to encode with: CODEC_VP8 for cameras; CODEC_H264 (on a hardware encoder, only in a build
	/// with USE_H264) or CODEC_VP9 for screens. CODEC_TILED is the old screen codec, kept for tests; 0 and 1
	/// match Settings::videoCodec. An unknown value, or H.264 in a build without it, falls back to VP8.
	static constexpr int CODEC_VP8   = 0;
	static constexpr int CODEC_TILED = 1;
	static constexpr int CODEC_H264  = 2;
	static constexpr int CODEC_VP9   = 3;
	void setCodec(int codec);
	int codec() const { return m_codec; }
	/// Whether the current codec is one of the whole-frame screen codecs, which share the still-screen
	/// handling in VideoEncodeCore::process().
	bool isScreenCodec() const { return m_codec == CODEC_VP9 || m_codec == CODEC_H264; }

	/// Size of the most recently encoded frame, or an empty size before the first one.
	QSize frameSize() const { return m_lastTargetSize; }

	/// Applies the capture and quality settings. Called before start() so the first frame already uses
	/// them; neither VP8 nor VP9 can change resolution mid-stream.
	void configure(int codec, unsigned int bitrateKbps, unsigned int framerate, int tileQuality, int tileSize);

	/**
	 * Encodes on a thread of its own instead of the caller's.
	 *
	 * Encoding is the most expensive thing the client does - VP9 takes several milliseconds a frame at 1080p
	 * even on several cores - and on the GUI thread it competed with painting the window and handling input:
	 * the client stuttered while sharing. Off it, a frame that arrives while the previous one is still being
	 * encoded replaces any frame already waiting, so the encoder always works on the newest picture and falls
	 * behind by dropping frames rather than by building up a delay.
	 *
	 * Off by default so that tests can drive a source and see the units synchronously. Must be set before
	 * the first start().
	 */
	void setThreadedEncoding(bool threaded);

signals:
	/// Emitted before encoding a resized image, so the owner can announce its new stream.
	void streamResized(unsigned int previousID, unsigned int streamID, QSize size);
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
	void onCaptureIdle(std::uint64_t captureTimestampUsec);

protected:
	/// Decides what a frame needs (a new stream on a resize, a keyframe) and hands it to the encoder.
	void encodeFrame(const QImage &frame, std::uint64_t captureTimestampUsec);
	/// Runs a job: directly, or on the encode thread (one at a time, newest waiting job wins).
	void submit(VideoEncodeCore::Job job);
	void dispatch(VideoEncodeCore::Job job);
	/// Emits a finished job's units, unless the stream they belong to has since been stopped.
	void deliver(const VideoEncodeCore::Result &result, std::uint64_t generation);
	/// Calls fn with the core, on the encode thread when there is one, in order with the encode jobs.
	void withCore(std::function< void(VideoEncodeCore &) > fn);
	QSize targetSizeFor(const QSize &source) const;

	std::unique_ptr< VideoSource > m_source;
	std::shared_ptr< VideoEncodeCore > m_core;

	QThread *m_encodeThread = nullptr;
	QObject *m_encodeWorker = nullptr;
	bool m_encodeBusy       = false;
	std::optional< VideoEncodeCore::Job > m_pendingJob;
	// Bumped on every start and stop, so that a job still being encoded when its stream stopped does not
	// emit units for a stream that no longer exists.
	std::uint64_t m_generation = 0;

	// CODEC_VP8 for a camera; CODEC_H264 or CODEC_VP9 for a screen. See VpxCodec.h for the measurements
	// behind VP9 and H264Codec.h for the hardware encoders.
	int m_codec = CODEC_VP8;
	/// From configure(): handed to each source as its maximum rate, see VideoSource::setMaxFramerate().
	unsigned int m_framerate = 0;

	// 0 is a valid stream id, so the first stream uses it and each subsequent start increments.
	std::uint32_t m_streamID = 0;
	bool m_everStarted       = false;
	bool m_streamIDSeeded    = false;

	bool m_forceKeyframe = true;
	std::function< std::uint32_t() > m_allocateStreamID;
	/// The last captured frame, as captured - for re-encoding on a keyframe request or as a keep-alive.
	QImage m_lastFrame;
	QSize m_lastTargetSize;
	QElapsedTimer m_captureActivity;
	std::uint64_t m_captureTimestamp    = 0;
	std::uint64_t m_lastEncodeTimestamp = 0;

	/// MUMBLE_VIDEO_STATS: what was encoded since the last stats line (logged at most every 2 s).
	struct EncodeStats {
		bool enabled = false;
		QElapsedTimer window;
		std::uint64_t frames           = 0;
		std::uint64_t dropped          = 0;
		std::uint64_t keyframes        = 0;
		std::uint64_t units            = 0;
		std::uint64_t bytes            = 0;
		std::uint64_t encodeUsec       = 0;
		std::uint64_t maxEncodeUsec    = 0;
		std::uint64_t maxUnitsPerFrame = 0;
	} m_stats;
	void noteEncodeStats(bool keyframe, std::size_t units, std::uint64_t bytes, std::uint64_t encodeUsec);
};

#endif // MUMBLE_MUMBLE_VIDEOBROADCASTER_H_
