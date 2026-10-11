// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_VPXCODEC_H_
#define MUMBLE_MUMBLE_VPXCODEC_H_

#include "VideoEncoder.h"

#include <QtGui/QImage>

#include <cstdint>
#include <vector>

struct vpx_codec_ctx;
struct vpx_image;

/// Which libvpx codec a VpxEncoder/VpxDecoder uses.
enum class VpxCodec {
	VP8,
	VP9,
};

/// What a VpxEncoder is encoding, which decides how it is tuned.
enum class VpxContent {
	/// A camera: every pixel changes every frame. VP8, with a periodic keyframe.
	Camera,
	/// A screen or window: mostly static, text-heavy, with occasional large motion (scrolling, video).
	/// VP9 in its screen-content mode, multi-threaded, keyframes essentially only on request.
	Screen,
};

/**
 * Real-time encoding with libvpx (VP8 or VP9).
 *
 * The reason it exists is bandwidth, and the gap is not marginal. TiledImage sends every changed tile as
 * a complete JPEG, so it has no temporal compression at all: a camera costs tens of megabits a second,
 * and a screen share costs the same the moment anything moves. Measured at 1920x1080, 30 fps: a page
 * being scrolled cost TiledImage 77 Mbit/s and a video playing in a browser 52 Mbit/s, against 3 Mbit/s
 * for VP9 in screen-content mode at equal or better picture quality (luma PSNR). libvpx predicts each frame
 * from the last and sends only the difference, which is the entire reason real-time video is affordable.
 *
 * What it costs in exchange:
 *
 *  - A third-party dependency. libvpx has to be linked, and on the platforms that ship a static binary
 *    that is a packaging commitment rather than a build flag.
 *  - Whole frames. Neither codec has an independently decodable sub-frame region, so one frame is one
 *    unit, and losing any fragment of it loses the whole frame until it is retransmitted; after a lost
 *    frame everything that referenced it is wrong until a keyframe.
 */
class VpxEncoder {
public:
	explicit VpxEncoder(VpxContent content = VpxContent::Camera);
	~VpxEncoder();

	VpxEncoder(const VpxEncoder &)            = delete;
	VpxEncoder &operator=(const VpxEncoder &) = delete;

	/// Retunes the encoder for different content. Takes effect on the next frame, which is a keyframe.
	void setContent(VpxContent content);
	VpxContent content() const { return m_content; }
	/// The codec the current content type is encoded with.
	VpxCodec codec() const;

	/**
	 * Target bitrate in kilobits per second. Rate control is what keeps a frame inside the transport's
	 * unit budget, so this is a correctness knob as much as a quality one.
	 */
	void setBitrate(unsigned int kbps);
	unsigned int bitrate() const { return m_bitrate; }

	/// Longest run of frames before a keyframe is forced regardless.
	void setKeyframeInterval(unsigned int frames);

	/**
	 * Nominal frame rate, which is what gives rate control a per-frame bit budget.
	 *
	 * Without this the encoder has no idea how much time a frame represents, and the target bitrate
	 * means nothing at all.
	 */
	void setFramerate(unsigned int fps);
	unsigned int framerate() const { return m_framerate; }

	/// Discards encoder state so the next frame is a keyframe.
	void reset();

	bool isValid() const { return m_context != nullptr; }

	/**
	 * Encodes one frame into a single unit covering the whole picture.
	 *
	 * @returns The units to send, which is either one or, when the frame produced no output, none.
	 *   A unit that would not fit the transport is dropped and counted rather than sent, because a
	 *   fragment of a frame nobody can reassemble is worse than nothing; the next frame is then forced to
	 *   be a keyframe, since everything after a dropped frame would reference it.
	 */
	std::vector< EncodedVideoUnit > encode(const QImage &frame, std::uint32_t streamID, std::uint64_t frameNumber,
										   std::uint64_t captureTimestampUsec, bool forceKeyframe = false);

	struct Stats {
		unsigned int framesEncoded   = 0;
		unsigned int keyframes       = 0;
		unsigned int droppedOversize = 0;
		std::size_t bytesEncoded     = 0;
		std::size_t lastFrameBytes   = 0;
		bool lastFrameWasKeyframe    = false;
	};

	const Stats &stats() const { return m_stats; }

protected:
	vpx_codec_ctx *m_context = nullptr;
	vpx_image *m_raw         = nullptr;

	VpxContent m_content = VpxContent::Camera;

	int m_width            = 0;
	int m_height           = 0;
	unsigned int m_bitrate = 800;

	// 60 for a camera, not a more compression-friendly 120 or higher: this bounds how long a viewer's
	// picture can stay frozen after a lost inter-frame, in the worst case where an explicit keyframe request
	// (see VideoGrid::onVideoUnitReceived()'s KEYFRAME_REREQUEST_AFTER_UNITS, and the server's own once-a-
	// second relay limit on it) is itself delayed or lost. A screen uses SCREEN_KEYFRAME_INTERVAL instead:
	// its keyframes are many times larger than a camera's, and lost frames are recovered by retransmission.
	unsigned int m_keyframeInterval = 60;

	unsigned int m_framerate = 30;
	bool m_forceKeyframe     = true;

	// Presentation timestamp in timebase units, advanced by one per frame. Deliberately not the caller's
	// capture timestamp: rate control needs a monotonic, evenly spaced clock, and a caller is free to
	// supply timestamps that are neither.
	std::int64_t m_pts = 0;

	Stats m_stats;

	/// A screen's keyframes are 50-120 KB at 1080p, against a few kilobytes for an ordinary frame, so they
	/// are sent when needed - a new viewer, a viewer who could not recover a lost frame - and otherwise only
	/// rarely, as a backstop: every 10 seconds at 30 fps.
	static constexpr unsigned int SCREEN_KEYFRAME_INTERVAL = 300;

	/// (Re)creates the encoder for a given frame size. Neither codec can change resolution mid-stream.
	bool configure(int width, int height);

	void destroy();
};

/**
 * Decodes the units produced by VpxEncoder.
 */
class VpxDecoder {
public:
	explicit VpxDecoder(VpxCodec codec = VpxCodec::VP8);
	~VpxDecoder();

	VpxDecoder(const VpxDecoder &)            = delete;
	VpxDecoder &operator=(const VpxDecoder &) = delete;

	bool isValid() const { return m_context != nullptr; }
	VpxCodec codec() const { return m_codec; }

	/**
	 * Decodes one unit.
	 *
	 * @returns The decoded frame, or a null image if the data was not decodable. A decoder that has not
	 *   yet seen a keyframe returns null for every frame until one arrives, which is the correct
	 *   behaviour rather than a failure: nothing before the first keyframe can be reconstructed.
	 */
	QImage decode(const std::vector< Mumble::Protocol::byte > &payload);

	/**
	 * Decodes one unit, converting it to an image only when @p image is given.
	 *
	 * Every frame has to be decoded - the next one is predicted from it - but only a frame that will be shown
	 * needs converting to RGB, which at 1080p costs about as much as decoding. A viewer that has fallen
	 * behind decodes the frames it will not show without converting them.
	 *
	 * @returns Whether the unit decoded.
	 */
	bool decode(const std::vector< Mumble::Protocol::byte > &payload, QImage *image);

protected:
	vpx_codec_ctx *m_context = nullptr;
	VpxCodec m_codec         = VpxCodec::VP8;
};

#endif // MUMBLE_MUMBLE_VPXCODEC_H_
