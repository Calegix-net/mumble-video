// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "H264Codec.h"

#include "VideoFragmentation.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <QtCore/QtGlobal>

#include <algorithm>
#include <cstring>

namespace {

int roundDownToEven(int value) {
	return value & ~1;
}

struct Backend {
	const char *name;
	/// For h264_mf: insist on a hardware transform rather than Windows' software one.
	bool hardwareOnly;
};

// Most capable first. Each is only tried if the FFmpeg build has it, and each fails to open quickly
// without its hardware, so a machine with none costs a few milliseconds once (see isAvailable()).
#ifdef Q_OS_WIN
constexpr Backend BACKENDS[] = { { "h264_nvenc", false }, { "h264_amf", false }, { "h264_mf", true } };
#else
constexpr Backend BACKENDS[] = { { "h264_vaapi", false }, { "h264_nvenc", false } };
#endif

/// FFmpeg reports every undecodable unit on stderr. Frames that cannot be decoded are routine here - a
/// viewer that joins mid-stream drops inter-frames until a keyframe - and are handled, so only fatal
/// errors are worth printing.
void quietFFmpegLogging() {
	static const bool once = [] {
		av_log_set_level(AV_LOG_FATAL);
		return true;
	}();
	Q_UNUSED(once);
}

void setOption(AVCodecContext *context, const char *key, const char *value) {
	// Encoder-private options differ between FFmpeg releases; one that does not exist is not a reason to
	// give up on the encoder, only on that tuning.
	av_opt_set(context->priv_data, key, value, 0);
}

/// Whether an Annex B access unit carries a sequence parameter set (NAL type 7).
bool hasSps(const std::uint8_t *data, std::size_t size) {
	for (std::size_t i = 0; i + 3 < size; ++i) {
		if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
			if ((data[i + 3] & 0x1f) == 7) {
				return true;
			}
			i += 2;
		}
	}
	return false;
}

} // namespace

H264Encoder::H264Encoder() {
	quietFFmpegLogging();
}

H264Encoder::~H264Encoder() {
	destroy();
}

void H264Encoder::setBitrate(unsigned int kbps) {
	kbps = std::max(100u, std::min(kbps, 50000u));
	if (kbps != m_bitrate) {
		m_bitrate = kbps;
		reset();
	}
}

void H264Encoder::setFramerate(unsigned int fps) {
	fps = std::max(1u, std::min(fps, 60u));
	if (fps != m_framerate) {
		m_framerate = fps;
		reset();
	}
}

void H264Encoder::setKeyframeInterval(unsigned int frames) {
	frames = std::max(1u, frames);
	if (frames != m_keyframeInterval) {
		m_keyframeInterval = frames;
		reset();
	}
}

void H264Encoder::reset() {
	destroy();
	m_forceKeyframe = true;
	m_failed        = false;
}

void H264Encoder::destroy() {
	if (m_context) {
		avcodec_free_context(&m_context);
	}
	av_frame_free(&m_frame);
	av_frame_free(&m_hwFrame);
	av_packet_free(&m_packet);
	av_buffer_unref(&m_hwDevice);
	if (m_sws) {
		sws_freeContext(m_sws);
		m_sws = nullptr;
	}
	m_inFlight.clear();
	m_width  = 0;
	m_height = 0;
	m_vaapi  = false;
	m_backend.clear();
}

bool H264Encoder::isAvailable() {
	return !availableBackend().isEmpty();
}

QString H264Encoder::availableBackend() {
	static const QString backend = [] {
		H264Encoder probe;
		if (probe.open(256, 144)) {
			qInfo("H264Encoder: hardware encoder %s available", qPrintable(probe.backendName()));
			return probe.backendName();
		}
		qInfo("H264Encoder: no hardware H.264 encoder opened; screen shares stay on tiled images");
		return QString();
	}();
	return backend;
}

bool H264Encoder::open(int width, int height) {
	destroy();

	for (const Backend &backend : BACKENDS) {
		if (openBackend(backend.name, backend.hardwareOnly, width, height)) {
			m_backend = QString::fromLatin1(backend.name);
			m_width   = width;
			m_height  = height;
			return true;
		}
		destroy();
	}

	return false;
}

bool H264Encoder::openBackend(const char *name, bool hardwareOnly, int width, int height) {
	const AVCodec *codec = avcodec_find_encoder_by_name(name);
	if (!codec) {
		return false;
	}

	m_context = avcodec_alloc_context3(codec);
	if (!m_context) {
		return false;
	}

	const int fps = static_cast< int >(m_framerate);

	m_context->width     = width;
	m_context->height    = height;
	m_context->time_base = AVRational{ 1, fps };
	m_context->framerate = AVRational{ fps, 1 };
	m_context->bit_rate  = static_cast< std::int64_t >(m_bitrate) * 1000;
	// Constant bitrate with a buffer of about three frames: low latency, and it is what keeps a keyframe from
	// being many times an inter-frame's size, which would land as one huge burst on every viewer.
	m_context->rc_max_rate    = m_context->bit_rate;
	m_context->rc_buffer_size = static_cast< int >(m_context->bit_rate * 3 / fps);
	m_context->gop_size       = static_cast< int >(m_keyframeInterval);
	// No B-frames: each one is a frame of added delay, and the receiver decodes strictly in order.
	m_context->max_b_frames = 0;
	m_context->pix_fmt      = AV_PIX_FMT_NV12;
	m_context->color_range  = AVCOL_RANGE_MPEG;
	m_context->colorspace   = AVCOL_SPC_BT470BG;

	const bool vaapi = std::strcmp(name, "h264_vaapi") == 0;

	if (std::strcmp(name, "h264_nvenc") == 0) {
		setOption(m_context, "preset", "p4");
		setOption(m_context, "tune", "ull");
		setOption(m_context, "rc", "cbr");
		setOption(m_context, "zerolatency", "1");
		setOption(m_context, "delay", "0");
		setOption(m_context, "forced-idr", "1");
	} else if (std::strcmp(name, "h264_amf") == 0) {
		setOption(m_context, "usage", "ultralowlatency");
		setOption(m_context, "rc", "cbr");
		setOption(m_context, "quality", "speed");
		setOption(m_context, "forced_idr", "1");
	} else if (std::strcmp(name, "h264_mf") == 0) {
		setOption(m_context, "scenario", "display_remoting");
		setOption(m_context, "rate_control", "cbr");
		if (hardwareOnly) {
			setOption(m_context, "hw_encoding", "1");
		}
	} else if (vaapi) {
		setOption(m_context, "rc_mode", "CBR");
		setOption(m_context, "async_depth", "1");

		if (av_hwdevice_ctx_create(&m_hwDevice, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0) < 0) {
			return false;
		}

		AVBufferRef *frames = av_hwframe_ctx_alloc(m_hwDevice);
		if (!frames) {
			return false;
		}

		auto *framesContext              = reinterpret_cast< AVHWFramesContext * >(frames->data);
		framesContext->format            = AV_PIX_FMT_VAAPI;
		framesContext->sw_format         = AV_PIX_FMT_NV12;
		framesContext->width             = width;
		framesContext->height            = height;
		framesContext->initial_pool_size = 4;

		if (av_hwframe_ctx_init(frames) < 0) {
			av_buffer_unref(&frames);
			return false;
		}

		m_context->hw_frames_ctx = frames;
		m_context->pix_fmt       = AV_PIX_FMT_VAAPI;
	}

	if (avcodec_open2(m_context, codec, nullptr) < 0) {
		return false;
	}

	m_frame  = av_frame_alloc();
	m_packet = av_packet_alloc();
	if (!m_frame || !m_packet) {
		return false;
	}

	m_frame->format = AV_PIX_FMT_NV12;
	m_frame->width  = width;
	m_frame->height = height;
	if (av_frame_get_buffer(m_frame, 0) < 0) {
		return false;
	}

	if (vaapi) {
		m_hwFrame = av_frame_alloc();
		if (!m_hwFrame) {
			return false;
		}
	}

	m_sws = sws_getContext(width, height, AV_PIX_FMT_BGRA, width, height, AV_PIX_FMT_NV12, SWS_POINT, nullptr, nullptr,
						   nullptr);
	if (!m_sws) {
		return false;
	}

	m_vaapi = vaapi;
	return true;
}

std::vector< EncodedVideoUnit > H264Encoder::encode(const QImage &frame, std::uint32_t streamID,
													std::uint64_t frameNumber, std::uint64_t captureTimestampUsec,
													bool forceKeyframe) {
	std::vector< EncodedVideoUnit > units;

	if (frame.isNull()) {
		return units;
	}

	const int width  = roundDownToEven(frame.width());
	const int height = roundDownToEven(frame.height());

	if (width < 16 || height < 16) {
		return units;
	}

	if (!m_context || width != m_width || height != m_height) {
		if (m_failed && width == m_width && height == m_height) {
			return units;
		}
		if (!open(width, height)) {
			qWarning("H264Encoder: no hardware encoder opened at %dx%d", width, height);
			m_failed = true;
			m_width  = width;
			m_height = height;
			return units;
		}
		m_failed        = false;
		m_forceKeyframe = true;
		qInfo("H264Encoder: %s at %dx%d, %u kbit/s, %u fps", qPrintable(m_backend), width, height, m_bitrate,
			  m_framerate);
	}

	const QImage source = frame.format() == QImage::Format_RGB32 || frame.format() == QImage::Format_ARGB32
							  ? frame
							  : frame.convertToFormat(QImage::Format_RGB32);

	if (av_frame_make_writable(m_frame) < 0) {
		return units;
	}

	const std::uint8_t *sourcePlanes[1] = { source.constBits() };
	const int sourceStrides[1]          = { static_cast< int >(source.bytesPerLine()) };
	sws_scale(m_sws, sourcePlanes, sourceStrides, 0, height, m_frame->data, m_frame->linesize);

	AVFrame *submit = m_frame;

	if (m_vaapi) {
		av_frame_unref(m_hwFrame);
		if (av_hwframe_get_buffer(m_context->hw_frames_ctx, m_hwFrame, 0) < 0
			|| av_hwframe_transfer_data(m_hwFrame, m_frame, 0) < 0) {
			return units;
		}
		submit = m_hwFrame;
	}

	const bool wantKeyframe = forceKeyframe || m_forceKeyframe;
	m_forceKeyframe         = false;

	submit->pts       = m_pts++;
	submit->pict_type = wantKeyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
	if (wantKeyframe) {
		submit->flags |= AV_FRAME_FLAG_KEY;
	} else {
		submit->flags &= ~AV_FRAME_FLAG_KEY;
	}

	m_inFlight.emplace_back(submit->pts, std::make_pair(frameNumber, captureTimestampUsec));
	// A frame the encoder silently dropped would otherwise sit here forever.
	while (m_inFlight.size() > 16) {
		m_inFlight.pop_front();
	}

	int sent = avcodec_send_frame(m_context, submit);
	if (sent == AVERROR(EAGAIN)) {
		drain(units, streamID);
		sent = avcodec_send_frame(m_context, submit);
	}

	if (sent < 0) {
		m_forceKeyframe = true;
		return units;
	}

	drain(units, streamID);
	return units;
}

void H264Encoder::drain(std::vector< EncodedVideoUnit > &units, std::uint32_t streamID) {
	while (avcodec_receive_packet(m_context, m_packet) == 0) {
		std::uint64_t frameNumber = 0;
		std::uint64_t timestamp   = 0;
		bool found                = false;

		while (!m_inFlight.empty() && m_inFlight.front().first <= m_packet->pts) {
			if (m_inFlight.front().first == m_packet->pts) {
				frameNumber = m_inFlight.front().second.first;
				timestamp   = m_inFlight.front().second.second;
				found       = true;
			}
			m_inFlight.pop_front();
		}

		if (!found) {
			av_packet_unref(m_packet);
			continue;
		}

		const bool isKeyframe = (m_packet->flags & AV_PKT_FLAG_KEY) != 0;

		std::vector< std::uint8_t > accessUnit;
		accessUnit.reserve(static_cast< std::size_t >(m_packet->size) + 256);

		// A viewer that joins mid-stream starts at a keyframe, and can only decode it with the parameter
		// sets in hand. Most encoders repeat them before every IDR; for one that put them in extradata
		// only, they are prepended here.
		if (isKeyframe && !hasSps(m_packet->data, static_cast< std::size_t >(m_packet->size)) && m_context->extradata
			&& m_context->extradata_size > 4 && m_context->extradata[0] == 0 && m_context->extradata[1] == 0) {
			accessUnit.insert(accessUnit.end(), m_context->extradata, m_context->extradata + m_context->extradata_size);
		}

		accessUnit.insert(accessUnit.end(), m_packet->data, m_packet->data + m_packet->size);
		av_packet_unref(m_packet);

		std::vector< EncodedVideoUnit > parts = split(accessUnit, streamID, frameNumber, timestamp, isKeyframe, m_width,
													  m_height, Mumble::Protocol::VideoFragmenter::maxUnitSize());

		if (parts.empty()) {
			++m_stats.droppedOversize;
			m_forceKeyframe = true;
			continue;
		}

		++m_stats.framesEncoded;
		m_stats.bytesEncoded += accessUnit.size();
		m_stats.lastFrameBytes = accessUnit.size();
		if (isKeyframe) {
			++m_stats.keyframes;
		}

		for (EncodedVideoUnit &part : parts) {
			units.push_back(std::move(part));
		}
	}
}

std::vector< EncodedVideoUnit > H264Encoder::split(const std::vector< std::uint8_t > &accessUnit,
												   std::uint32_t streamID, std::uint64_t frameNumber,
												   std::uint64_t captureTimestampUsec, bool isKeyframe, int width,
												   int height, std::size_t maxUnitSize) {
	std::vector< EncodedVideoUnit > units;

	if (accessUnit.empty() || maxUnitSize == 0) {
		return units;
	}

	const std::size_t count = (accessUnit.size() + maxUnitSize - 1) / maxUnitSize;
	if (count > MAX_UNITS_PER_FRAME) {
		return units;
	}

	units.reserve(count);

	for (std::size_t i = 0; i < count; ++i) {
		const std::size_t begin = i * maxUnitSize;
		const std::size_t end   = std::min(accessUnit.size(), begin + maxUnitSize);

		EncodedVideoUnit unit;
		unit.header.streamID             = streamID;
		unit.header.frameNumber          = frameNumber;
		unit.header.unitID               = static_cast< std::uint32_t >(i);
		unit.header.captureTimestampUsec = captureTimestampUsec;
		unit.header.isKeyframe           = isKeyframe;
		unit.header.isFrameEnd           = i + 1 == count;
		// Part index and count; see the class comment.
		unit.header.x      = static_cast< std::uint32_t >(i);
		unit.header.y      = static_cast< std::uint32_t >(count);
		unit.header.width  = static_cast< std::uint32_t >(width);
		unit.header.height = static_cast< std::uint32_t >(height);
		unit.payload.assign(accessUnit.begin() + static_cast< std::ptrdiff_t >(begin),
							accessUnit.begin() + static_cast< std::ptrdiff_t >(end));

		units.push_back(std::move(unit));
	}

	return units;
}

bool H264FrameAssembler::add(std::uint64_t frameNumber, bool isKeyframe, unsigned int index, unsigned int count,
							 const QByteArray &part, Complete &out) {
	if (count == 0 || count > H264Encoder::MAX_UNITS_PER_FRAME || index >= count || part.isEmpty()) {
		return false;
	}

	if (std::find(m_completed.begin(), m_completed.end(), frameNumber) != m_completed.end()) {
		return false;
	}

	auto it = std::find_if(m_pending.begin(), m_pending.end(),
						   [frameNumber](const auto &entry) { return entry.first == frameNumber; });

	if (it == m_pending.end()) {
		if (m_pending.size() >= MAX_PENDING_FRAMES) {
			// The oldest incomplete frame is the least likely ever to complete.
			auto oldest = std::min_element(m_pending.begin(), m_pending.end(),
										   [](const auto &a, const auto &b) { return a.first < b.first; });
			m_pending.erase(oldest);
		}

		Pending pending;
		pending.count = count;
		pending.parts.resize(count);
		m_pending.emplace_back(frameNumber, std::move(pending));
		it = m_pending.end() - 1;
	}

	Pending &pending = it->second;

	if (pending.count != count || !pending.parts[index].isEmpty()) {
		return false;
	}

	pending.parts[index] = part;
	pending.isKeyframe   = pending.isKeyframe || isKeyframe;
	++pending.have;

	if (pending.have < pending.count) {
		return false;
	}

	out.frameNumber = frameNumber;
	out.isKeyframe  = pending.isKeyframe;
	out.accessUnit.clear();
	for (const QByteArray &piece : pending.parts) {
		out.accessUnit.append(piece);
	}

	// An older frame stays pending even though a newer one just completed: the decoding path holds the
	// newer one back and asks for the older one's missing parts again, so they must still be accepted.
	m_pending.erase(it);

	m_completed.push_back(frameNumber);
	while (m_completed.size() > MAX_COMPLETED_REMEMBERED) {
		m_completed.pop_front();
	}

	return true;
}

std::vector< unsigned int > H264FrameAssembler::missingParts(std::uint64_t frameNumber) const {
	std::vector< unsigned int > missing;

	for (const auto &entry : m_pending) {
		if (entry.first != frameNumber) {
			continue;
		}
		for (unsigned int i = 0; i < entry.second.count; ++i) {
			if (entry.second.parts[i].isEmpty()) {
				missing.push_back(i);
			}
		}
	}

	return missing;
}

void H264FrameAssembler::clear() {
	m_pending.clear();
	m_completed.clear();
}

H264Decoder::H264Decoder() {
	quietFFmpegLogging();

	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
	if (!codec) {
		return;
	}

	m_context = avcodec_alloc_context3(codec);
	if (!m_context) {
		return;
	}

	// Output each frame as soon as it is decoded rather than holding some back for reordering, which a
	// stream without B-frames never needs. Slice threads only, for the same reason: frame threading
	// delays every picture by a frame per thread.
	m_context->flags |= AV_CODEC_FLAG_LOW_DELAY;
	m_context->thread_type  = FF_THREAD_SLICE;
	m_context->thread_count = 0;

	if (avcodec_open2(m_context, codec, nullptr) < 0) {
		avcodec_free_context(&m_context);
		return;
	}

	m_frame  = av_frame_alloc();
	m_packet = av_packet_alloc();

	if (!m_frame || !m_packet) {
		avcodec_free_context(&m_context);
	}
}

H264Decoder::~H264Decoder() {
	avcodec_free_context(&m_context);
	av_frame_free(&m_frame);
	av_packet_free(&m_packet);
	if (m_sws) {
		sws_freeContext(m_sws);
	}
}

QImage H264Decoder::decode(const QByteArray &accessUnit) {
	if (!m_context || accessUnit.isEmpty()) {
		return QImage();
	}

	if (av_new_packet(m_packet, static_cast< int >(accessUnit.size())) < 0) {
		return QImage();
	}
	std::memcpy(m_packet->data, accessUnit.constData(), static_cast< std::size_t >(accessUnit.size()));

	const int sent = avcodec_send_packet(m_context, m_packet);
	av_packet_unref(m_packet);

	if (sent < 0) {
		return QImage();
	}

	QImage image;

	while (avcodec_receive_frame(m_context, m_frame) == 0) {
		const bool damaged = m_frame->decode_error_flags != 0 || (m_frame->flags & AV_FRAME_FLAG_CORRUPT) != 0;

		if (!damaged && m_frame->width > 0 && m_frame->height > 0) {
			m_sws = sws_getCachedContext(m_sws, m_frame->width, m_frame->height,
										 static_cast< AVPixelFormat >(m_frame->format), m_frame->width, m_frame->height,
										 AV_PIX_FMT_BGRA, SWS_POINT, nullptr, nullptr, nullptr);

			if (m_sws) {
				QImage out(m_frame->width, m_frame->height, QImage::Format_RGB32);
				std::uint8_t *destinationPlanes[1] = { out.bits() };
				const int destinationStrides[1]    = { static_cast< int >(out.bytesPerLine()) };

				sws_scale(m_sws, m_frame->data, m_frame->linesize, 0, m_frame->height, destinationPlanes,
						  destinationStrides);
				image = out;
			}
		} else {
			image = QImage();
		}

		av_frame_unref(m_frame);
	}

	return image;
}
