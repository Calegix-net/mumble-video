// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_VP8CODEC_H_
#define MUMBLE_MUMBLE_VP8CODEC_H_

#include "VideoEncoder.h"

#include <QtGui/QImage>

#include <cstdint>
#include <vector>

struct vpx_codec_ctx;
struct vpx_image;

/**
 * VP8 encoding, as an alternative to the tiled-JPEG codec.
 *
 * The reason it exists is bandwidth, and the gap is not marginal. TiledImage sends every changed tile as
 * a complete JPEG, so it has no temporal compression at all: measured against synthetic camera content,
 * a 480p stream costs tens of megabits a second, because every frame is effectively a still photograph.
 * VP8 predicts each frame from the last and sends only the difference, which is the entire reason
 * real-time video is affordable.
 *
 * What it costs in exchange:
 *
 *  - A third-party dependency. libvpx has to be linked, and on the platforms that ship a static binary
 *    that is a packaging commitment rather than a build flag.
 *  - Whole frames. VP8 has no independently decodable sub-frame region -- its token partitions are
 *    separate entropy streams that are useless without the frame header -- so one frame is one unit, and
 *    losing any fragment of it loses the whole frame. TiledImage degrades into a partially stale picture
 *    under loss; VP8 degrades into a dropped frame, and after a dropped frame everything that referenced
 *    it is wrong until the next keyframe.
 *
 * Neither codec is strictly better. TiledImage remains the right choice for a mostly-static screen,
 * where dirty-tile detection sends literally nothing; VP8 is the right choice for a camera, where every
 * pixel changes every frame and TiledImage is ruinous.
 */
class VP8Encoder {
public:
	VP8Encoder();
	~VP8Encoder();

	VP8Encoder(const VP8Encoder &)            = delete;
	VP8Encoder &operator=(const VP8Encoder &) = delete;

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
	 *   fragment of a frame nobody can reassemble is worse than nothing.
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

	int m_width                     = 0;
	int m_height                    = 0;
	unsigned int m_bitrate          = 800;
	unsigned int m_keyframeInterval = 120;
	unsigned int m_framerate        = 30;
	bool m_forceKeyframe            = true;

	// Presentation timestamp in timebase units, advanced by one per frame. Deliberately not the caller's
	// capture timestamp: rate control needs a monotonic, evenly spaced clock, and a caller is free to
	// supply timestamps that are neither.
	std::int64_t m_pts = 0;

	Stats m_stats;

	/// (Re)creates the encoder for a given frame size. VP8 cannot change resolution mid-stream.
	bool configure(int width, int height);

	void destroy();
};

/**
 * Decodes the units produced by VP8Encoder.
 */
class VP8Decoder {
public:
	VP8Decoder();
	~VP8Decoder();

	VP8Decoder(const VP8Decoder &)            = delete;
	VP8Decoder &operator=(const VP8Decoder &) = delete;

	bool isValid() const { return m_context != nullptr; }

	/**
	 * Decodes one unit.
	 *
	 * @returns The decoded frame, or a null image if the data was not decodable. A decoder that has not
	 *   yet seen a keyframe returns null for every frame until one arrives, which is the correct
	 *   behaviour rather than a failure: nothing before the first keyframe can be reconstructed.
	 */
	QImage decode(const std::vector< Mumble::Protocol::byte > &payload);

protected:
	vpx_codec_ctx *m_context = nullptr;
};

#endif // MUMBLE_MUMBLE_VP8CODEC_H_
