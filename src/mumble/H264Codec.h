// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_H264CODEC_H_
#define MUMBLE_MUMBLE_H264CODEC_H_

#include "VideoEncoder.h"

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtGui/QImage>

#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

struct AVBufferRef;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

/**
 * H.264 encoding on the graphics card, for screen shares that move.
 *
 * TiledImage sends each changed tile as a complete JPEG with no temporal compression, which is right for a
 * desktop that barely changes and ruinous for a film or a game: every tile changes every frame, so every
 * frame is a full-screen still photograph, encoded on the sender's GUI thread. Measured on a real share of
 * a film that came to about 12 Mbit/s at an uneven 20 frames a second. A hardware H.264 encoder does the
 * same picture at a steady frame rate for a fraction of the bits and almost none of the CPU, which is how
 * Discord's screen sharing is smooth.
 *
 * Only hardware encoders are tried - NVENC, AMF and Media Foundation on Windows (the last covers Intel
 * Quick Sync, and falls back to Windows' own software encoder), VA-API and NVENC on Linux - through a
 * small FFmpeg build. When none opens, isAvailable() is false and the caller keeps TiledImage.
 *
 * Decoding is FFmpeg's software H.264 decoder everywhere, which comfortably keeps up with one 1080p share.
 *
 * A frame is one access unit, which at screen resolutions is regularly larger than the transport's unit
 * budget (VideoFragmenter::maxUnitSize()), keyframes always. So an encoded frame is split into as many
 * units as it needs, numbered by unitID, and since a whole-frame codec has no use for a tile position the
 * unit header's x and y carry this part's index and the frame's part count. The receiver joins the parts
 * back into the access unit before decoding (see H264FrameAssembler).
 */
class H264Encoder {
public:
	H264Encoder();
	~H264Encoder();

	H264Encoder(const H264Encoder &)            = delete;
	H264Encoder &operator=(const H264Encoder &) = delete;

	/// Target bitrate in kilobits per second. Rate control keeps the stream at about this, with a buffer of
	/// a couple of frames so a keyframe cannot balloon far past the average.
	void setBitrate(unsigned int kbps);
	unsigned int bitrate() const { return m_bitrate; }

	void setFramerate(unsigned int fps);
	unsigned int framerate() const { return m_framerate; }

	/// Longest run of frames before the encoder inserts a keyframe on its own.
	void setKeyframeInterval(unsigned int frames);

	/// Discards encoder state so the next frame is a keyframe at whatever size it has.
	void reset();

	/// Which encoder is open (h264_nvenc, h264_mf, ...), or empty before the first frame or when none opened.
	QString backendName() const { return m_backend; }

	/**
	 * Whether any hardware H.264 encoder opens on this machine. Tried once, at a small size, and
	 * remembered: the answer depends on the GPU and its driver, which do not change while running.
	 */
	static bool isAvailable();

	/// The encoder isAvailable() found (h264_nvenc, ...), or empty when none opened.
	static QString availableBackend();

	/**
	 * Encodes one frame.
	 *
	 * @returns The units carrying it, in order, or none when the encoder produced no output for this
	 *   input. A hardware encoder may hand a frame back one call late; its units then carry the frame
	 *   number and timestamp the frame was submitted with, not the current call's.
	 */
	std::vector< EncodedVideoUnit > encode(const QImage &frame, std::uint32_t streamID, std::uint64_t frameNumber,
										   std::uint64_t captureTimestampUsec, bool forceKeyframe = false);

	/// Most units one frame may be split into. Rate control keeps a frame well under this; a frame still
	/// larger is dropped, and the next one forced to a keyframe, rather than sent incomplete.
	static constexpr unsigned int MAX_UNITS_PER_FRAME = 32;

	struct Stats {
		unsigned int framesEncoded   = 0;
		unsigned int keyframes       = 0;
		unsigned int droppedOversize = 0;
		std::size_t bytesEncoded     = 0;
		std::size_t lastFrameBytes   = 0;
	};

	const Stats &stats() const { return m_stats; }

	/// Splits one encoded access unit into transport-sized units (exposed for tests).
	static std::vector< EncodedVideoUnit > split(const std::vector< std::uint8_t > &accessUnit, std::uint32_t streamID,
												 std::uint64_t frameNumber, std::uint64_t captureTimestampUsec,
												 bool isKeyframe, int width, int height, std::size_t maxUnitSize);

protected:
	bool open(int width, int height);
	bool openBackend(const char *name, bool hardwareOnly, int width, int height);
	void destroy();
	void drain(std::vector< EncodedVideoUnit > &units, std::uint32_t streamID);

	AVCodecContext *m_context = nullptr;
	AVBufferRef *m_hwDevice   = nullptr;
	AVFrame *m_frame          = nullptr;
	AVFrame *m_hwFrame        = nullptr;
	AVPacket *m_packet        = nullptr;
	SwsContext *m_sws         = nullptr;
	bool m_vaapi              = false;
	QString m_backend;

	int m_width              = 0;
	int m_height             = 0;
	unsigned int m_bitrate   = 6000;
	unsigned int m_framerate = 30;
	// Four seconds at 30 fps (VideoBroadcaster::configure() sets it from the frame rate): this bounds how long
	// a viewer stays frozen after a loss whose explicit keyframe request was itself lost.
	unsigned int m_keyframeInterval = 120;
	bool m_forceKeyframe            = true;
	bool m_failed                   = false;

	std::int64_t m_pts = 0;
	/// Frame number and capture timestamp of each frame still inside the encoder, by pts.
	std::deque< std::pair< std::int64_t, std::pair< std::uint64_t, std::uint64_t > > > m_inFlight;

	Stats m_stats;
};

/**
 * Joins the parts of H.264 frames back into access units, on the receiving side.
 *
 * Parts arrive as separate units and can arrive out of order. A frame is handed on once every part of it
 * is here, whatever order frames complete in: putting frames back in order, and asking again for one
 * that is missing parts (see missingParts()), is the decoding path's job, exactly as for a lost VP8 frame.
 */
class H264FrameAssembler {
public:
	struct Complete {
		std::uint64_t frameNumber = 0;
		bool isKeyframe           = false;
		QByteArray accessUnit;
	};

	/**
	 * Adds one part.
	 *
	 * @returns The frame this part completed, if it did. A part that is malformed, duplicates one already
	 *   held, or belongs to a frame already handed on is ignored.
	 */
	bool add(std::uint64_t frameNumber, bool isKeyframe, unsigned int index, unsigned int count, const QByteArray &part,
			 Complete &out);

	/// The parts still missing from an incomplete frame, if any part of it has arrived.
	std::vector< unsigned int > missingParts(std::uint64_t frameNumber) const;

	void clear();

	/// Frames held incomplete at once. A sender keeps a handful in flight at most; more means loss.
	static constexpr std::size_t MAX_PENDING_FRAMES = 8;

protected:
	struct Pending {
		unsigned int count = 0;
		unsigned int have  = 0;
		bool isKeyframe    = false;
		std::vector< QByteArray > parts;
	};

	std::vector< std::pair< std::uint64_t, Pending > > m_pending;
	/// Frames already handed on, so a late duplicate part cannot start assembling one of them again.
	std::deque< std::uint64_t > m_completed;
	static constexpr std::size_t MAX_COMPLETED_REMEMBERED = 64;
};

/**
 * Decodes the access units H264Encoder produced, once H264FrameAssembler has joined them.
 */
class H264Decoder {
public:
	H264Decoder();
	~H264Decoder();

	H264Decoder(const H264Decoder &)            = delete;
	H264Decoder &operator=(const H264Decoder &) = delete;

	bool isValid() const { return m_context != nullptr; }

	/**
	 * Decodes one access unit.
	 *
	 * @returns The decoded frame, or a null image if it did not decode or decoded with errors - an
	 *   inter-frame whose reference was lost decodes into a damaged picture, which is worse than the
	 *   previous frame staying up until a keyframe repairs it.
	 */
	QImage decode(const QByteArray &accessUnit);

protected:
	AVCodecContext *m_context = nullptr;
	AVFrame *m_frame          = nullptr;
	AVPacket *m_packet        = nullptr;
	SwsContext *m_sws         = nullptr;
};

#endif // MUMBLE_MUMBLE_H264CODEC_H_
