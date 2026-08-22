// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "VP8Codec.h"

#include "VideoFragmentation.h"

#include <vpx/vp8cx.h>
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>
#include <vpx/vpx_encoder.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace {

/// VP8 works in 4:2:0, so both dimensions must be even.
int roundDownToEven(int value) {
	return value & ~1;
}

/// BT.601 full-range RGB to I420. Written out rather than pulled from libyuv because one more
/// dependency for one colour conversion is a poor trade, and this is not the expensive part of encoding.
void rgbToI420(const QImage &source, vpx_image_t *destination) {
	const int width  = static_cast< int >(destination->d_w);
	const int height = static_cast< int >(destination->d_h);

	unsigned char *yPlane = destination->planes[VPX_PLANE_Y];
	unsigned char *uPlane = destination->planes[VPX_PLANE_U];
	unsigned char *vPlane = destination->planes[VPX_PLANE_V];

	for (int y = 0; y < height; ++y) {
		const QRgb *scan = reinterpret_cast< const QRgb * >(source.constScanLine(y));

		for (int x = 0; x < width; ++x) {
			const int r = qRed(scan[x]);
			const int g = qGreen(scan[x]);
			const int b = qBlue(scan[x]);

			yPlane[y * destination->stride[VPX_PLANE_Y] + x] =
				static_cast< unsigned char >(std::clamp((77 * r + 150 * g + 29 * b) >> 8, 0, 255));
		}
	}

	// Chroma is subsampled by averaging each 2x2 block, which is what 4:2:0 means and avoids the
	// aliasing that point-sampling one corner would produce.
	for (int y = 0; y + 1 < height; y += 2) {
		const QRgb *top    = reinterpret_cast< const QRgb * >(source.constScanLine(y));
		const QRgb *bottom = reinterpret_cast< const QRgb * >(source.constScanLine(y + 1));

		for (int x = 0; x + 1 < width; x += 2) {
			const int r = (qRed(top[x]) + qRed(top[x + 1]) + qRed(bottom[x]) + qRed(bottom[x + 1])) / 4;
			const int g = (qGreen(top[x]) + qGreen(top[x + 1]) + qGreen(bottom[x]) + qGreen(bottom[x + 1])) / 4;
			const int b = (qBlue(top[x]) + qBlue(top[x + 1]) + qBlue(bottom[x]) + qBlue(bottom[x + 1])) / 4;

			const int u = ((-43 * r - 85 * g + 128 * b) >> 8) + 128;
			const int v = ((128 * r - 107 * g - 21 * b) >> 8) + 128;

			uPlane[(y / 2) * destination->stride[VPX_PLANE_U] + (x / 2)] =
				static_cast< unsigned char >(std::clamp(u, 0, 255));
			vPlane[(y / 2) * destination->stride[VPX_PLANE_V] + (x / 2)] =
				static_cast< unsigned char >(std::clamp(v, 0, 255));
		}
	}
}

QImage i420ToRgb(const vpx_image_t *source) {
	const int width  = static_cast< int >(source->d_w);
	const int height = static_cast< int >(source->d_h);

	QImage image(width, height, QImage::Format_RGB32);

	for (int y = 0; y < height; ++y) {
		QRgb *scan = reinterpret_cast< QRgb * >(image.scanLine(y));

		const unsigned char *yRow = source->planes[VPX_PLANE_Y] + y * source->stride[VPX_PLANE_Y];
		const unsigned char *uRow = source->planes[VPX_PLANE_U] + (y / 2) * source->stride[VPX_PLANE_U];
		const unsigned char *vRow = source->planes[VPX_PLANE_V] + (y / 2) * source->stride[VPX_PLANE_V];

		for (int x = 0; x < width; ++x) {
			const int luma = yRow[x];
			const int cb   = uRow[x / 2] - 128;
			const int cr   = vRow[x / 2] - 128;

			const int r = luma + ((91881 * cr) >> 16);
			const int g = luma - ((22554 * cb + 46802 * cr) >> 16);
			const int b = luma + ((116130 * cb) >> 16);

			scan[x] = qRgb(std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b, 0, 255));
		}
	}

	return image;
}

} // namespace

VP8Encoder::VP8Encoder() = default;

VP8Encoder::~VP8Encoder() {
	destroy();
}

void VP8Encoder::destroy() {
	if (m_context) {
		vpx_codec_destroy(m_context);
		delete m_context;
		m_context = nullptr;
	}

	if (m_raw) {
		vpx_img_free(m_raw);
		m_raw = nullptr;
	}

	m_width  = 0;
	m_height = 0;
}

void VP8Encoder::setBitrate(unsigned int kbps) {
	m_bitrate = std::clamp(kbps, 50u, 20000u);

	// Rate control is set at configuration time, so a change takes effect on the next reconfigure.
	destroy();
}

void VP8Encoder::setKeyframeInterval(unsigned int frames) {
	m_keyframeInterval = std::max(1u, frames);
	destroy();
}

void VP8Encoder::setFramerate(unsigned int fps) {
	m_framerate = std::clamp(fps, 1u, 120u);
	destroy();
}

void VP8Encoder::reset() {
	m_forceKeyframe = true;
}

bool VP8Encoder::configure(int width, int height) {
	destroy();

	width  = roundDownToEven(width);
	height = roundDownToEven(height);

	if (width <= 0 || height <= 0) {
		return false;
	}

	vpx_codec_enc_cfg_t cfg;

	if (vpx_codec_enc_config_default(vpx_codec_vp8_cx(), &cfg, 0) != VPX_CODEC_OK) {
		return false;
	}

	cfg.g_w = static_cast< unsigned int >(width);
	cfg.g_h = static_cast< unsigned int >(height);

	// One tick per frame. The obvious alternative -- a microsecond timebase driven by capture timestamps
	// -- is what this originally did, and it silently disabled rate control: a caller that passes the same
	// timestamp for every frame, or a duration of one microsecond, tells libvpx the frames are a
	// millionth of a second apart, so the per-frame bit budget becomes effectively infinite and the
	// target bitrate is ignored. Counting in frames cannot go wrong that way.
	cfg.g_timebase.num = 1;
	cfg.g_timebase.den = static_cast< int >(m_framerate);

	cfg.rc_target_bitrate = m_bitrate;

	// Constant bitrate rather than quality-targeted. For a live call, a predictable rate matters more
	// than uniform quality, and it is what keeps a frame inside the transport's unit budget.
	cfg.rc_end_usage = VPX_CBR;

	// No lag: every frame must be available to send immediately. Any lookahead would trade latency for
	// compression, which is the wrong trade in a conversation.
	cfg.g_lag_in_frames = 0;

	// There is no retransmission under this transport, so a lost frame must not poison everything after
	// it any longer than necessary.
	cfg.g_error_resilient = VPX_ERROR_RESILIENT_DEFAULT;

	cfg.kf_mode     = VPX_KF_AUTO;
	cfg.kf_max_dist = m_keyframeInterval;

	cfg.g_threads = 1;

	auto *context = new vpx_codec_ctx_t();

	if (vpx_codec_enc_init(context, vpx_codec_vp8_cx(), &cfg, 0) != VPX_CODEC_OK) {
		delete context;

		return false;
	}

	m_context = context;

	// Bounds how much larger a keyframe may be than the per-frame budget. Without this a keyframe can be
	// many times the average frame and overflow the transport's maximum unit, which would drop it -- and
	// a dropped keyframe means the picture never starts.
	vpx_codec_control(m_context, VP8E_SET_MAX_INTRA_BITRATE_PCT, 400);

	// Trades compression for speed. Realtime encoding on a call is worth more than a few percent of
	// bitrate, and this keeps the encoder off the critical path.
	vpx_codec_control(m_context, VP8E_SET_CPUUSED, 8);

	m_raw = vpx_img_alloc(nullptr, VPX_IMG_FMT_I420, static_cast< unsigned int >(width),
						  static_cast< unsigned int >(height), 1);

	if (!m_raw) {
		destroy();

		return false;
	}

	m_width         = width;
	m_height        = height;
	m_forceKeyframe = true;
	m_pts           = 0;

	return true;
}

std::vector< EncodedVideoUnit > VP8Encoder::encode(const QImage &frame, std::uint32_t streamID,
												   std::uint64_t frameNumber, std::uint64_t captureTimestampUsec,
												   bool forceKeyframe) {
	std::vector< EncodedVideoUnit > units;

	if (frame.isNull()) {
		return units;
	}

	const int width  = roundDownToEven(frame.width());
	const int height = roundDownToEven(frame.height());

	// VP8 cannot change resolution within a stream, so a resize means a fresh encoder and a keyframe.
	if (width != m_width || height != m_height) {
		if (!configure(width, height)) {
			return units;
		}
	}

	if (!m_context || !m_raw) {
		return units;
	}

	const QImage source = frame.format() == QImage::Format_RGB32 ? frame : frame.convertToFormat(QImage::Format_RGB32);

	rgbToI420(source, m_raw);

	const bool wantKeyframe = forceKeyframe || m_forceKeyframe;

	// One frame's worth of the timebase, so rate control has a well-defined budget per frame.
	if (vpx_codec_encode(m_context, m_raw, static_cast< vpx_codec_pts_t >(m_pts), 1,
						 wantKeyframe ? VPX_EFLAG_FORCE_KF : 0, VPX_DL_REALTIME)
		!= VPX_CODEC_OK) {
		return units;
	}

	m_pts++;

	m_forceKeyframe = false;

	vpx_codec_iter_t iter         = nullptr;
	const vpx_codec_cx_pkt_t *pkt = nullptr;

	while ((pkt = vpx_codec_get_cx_data(m_context, &iter)) != nullptr) {
		if (pkt->kind != VPX_CODEC_CX_FRAME_PKT) {
			continue;
		}

		const auto *begin      = static_cast< const Mumble::Protocol::byte * >(pkt->data.frame.buf);
		const std::size_t size = pkt->data.frame.sz;

		const bool isKeyframe = (pkt->data.frame.flags & VPX_FRAME_IS_KEY) != 0;

		m_stats.lastFrameBytes       = size;
		m_stats.lastFrameWasKeyframe = isKeyframe;

		if (size > Mumble::Protocol::VideoFragmenter::maxUnitSize()) {
			// Sending part of a frame nobody can reassemble is worse than sending nothing, and it would
			// consume the receiver's reassembly budget on the way. Counted so this is visible rather
			// than mysterious.
			m_stats.droppedOversize++;

			continue;
		}

		EncodedVideoUnit unit;
		unit.header.streamID             = streamID;
		unit.header.frameNumber          = frameNumber;
		unit.header.unitID               = 0;
		unit.header.captureTimestampUsec = captureTimestampUsec;
		unit.header.isKeyframe           = isKeyframe;
		unit.header.isFrameEnd           = true;
		unit.header.x                    = 0;
		unit.header.y                    = 0;
		unit.header.width                = static_cast< std::uint32_t >(m_width);
		unit.header.height               = static_cast< std::uint32_t >(m_height);
		unit.payload.assign(begin, begin + size);

		m_stats.framesEncoded++;
		m_stats.bytesEncoded += size;

		if (isKeyframe) {
			m_stats.keyframes++;
		}

		units.push_back(std::move(unit));
	}

	return units;
}

VP8Decoder::VP8Decoder() {
	auto *context = new vpx_codec_ctx_t();

	if (vpx_codec_dec_init(context, vpx_codec_vp8_dx(), nullptr, 0) != VPX_CODEC_OK) {
		delete context;

		return;
	}

	m_context = context;
}

VP8Decoder::~VP8Decoder() {
	if (m_context) {
		vpx_codec_destroy(m_context);
		delete m_context;
	}
}

QImage VP8Decoder::decode(const std::vector< Mumble::Protocol::byte > &payload) {
	if (!m_context || payload.empty()) {
		return QImage();
	}

	if (vpx_codec_decode(m_context, payload.data(), static_cast< unsigned int >(payload.size()), nullptr, 0)
		!= VPX_CODEC_OK) {
		return QImage();
	}

	vpx_codec_iter_t iter  = nullptr;
	const vpx_image_t *img = vpx_codec_get_frame(m_context, &iter);

	if (!img) {
		return QImage();
	}

	// Decoding an inter-frame whose reference is missing does not fail - libvpx returns a plausible
	// corrupted image. The frame-continuity tracking upstream is the real defence; this catches what
	// it cannot: a keyframe that itself arrived damaged, or any bookkeeping slip. Costs one control
	// call per frame.
	int corrupted = 0;

	if (vpx_codec_control(m_context, VP8D_GET_FRAME_CORRUPTED, &corrupted) == VPX_CODEC_OK && corrupted) {
		return QImage();
	}

	return i420ToRgb(img);
}
