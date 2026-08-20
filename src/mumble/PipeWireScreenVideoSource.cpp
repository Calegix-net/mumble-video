// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "PipeWireScreenVideoSource.h"

#include <QtCore/QMutexLocker>
#include <QtCore/QTimer>

// CMake adds PipeWire's include directories with SYSTEM, which is what makes these compile at all:
// they are C headers built on GNU statement expressions and implicit conversions that this project's
// -Wpedantic -Wsign-conversion -Werror rejects outright. Worth knowing because the failure is
// unreadable - the macros using those extensions fail to expand and the errors that surface are
// "SPA_ROUND_UP was not declared", pointing inside PipeWire rather than at the warning flags.
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/utils/result.h>

#include <cstring>

namespace {

/// PipeWire has to be initialised once per process before anything else in the library is called.
void ensurePipeWireInitialised() {
	static bool initialised = false;

	if (!initialised) {
		pw_init(nullptr, nullptr);
		initialised = true;
	}
}

/**
 * Converts one row of a captured buffer to ARGB32.
 *
 * The portal negotiates BGRA/RGBA/BGRx/RGBx, and which one arrives depends on the compositor. QImage's
 * Format_ARGB32 is BGRA in memory on little-endian, so BGRA is a straight copy and the rest are
 * per-pixel swaps. Alpha from a screen capture is not meaningful - a compositor may hand back zero in
 * it - so the x-variants are forced opaque and the alpha ones are too.
 */
void convertRow(std::uint32_t spaFormat, const std::uint8_t *src, QRgb *dst, int width) {
	switch (spaFormat) {
		case SPA_VIDEO_FORMAT_BGRA:
		case SPA_VIDEO_FORMAT_BGRx:
			for (int x = 0; x < width; ++x) {
				const std::uint8_t *p = src + 4 * x;
				dst[x]                = qRgb(p[2], p[1], p[0]);
			}
			break;
		case SPA_VIDEO_FORMAT_RGBA:
		case SPA_VIDEO_FORMAT_RGBx:
			for (int x = 0; x < width; ++x) {
				const std::uint8_t *p = src + 4 * x;
				dst[x]                = qRgb(p[0], p[1], p[2]);
			}
			break;
		default:
			// Never negotiated, so unreachable; leaving the row untouched is better than reading it as
			// a format it is not.
			break;
	}
}

} // namespace

namespace {

// File-static rather than class members, so their PipeWire-typed signatures stay out of the header.
void streamStateChangedTrampoline(void *data, pw_stream_state, pw_stream_state state, const char *error) {
	static_cast< PipeWireScreenVideoSource * >(data)->onStreamStateChanged(static_cast< int >(state), error);
}

void streamParamChangedTrampoline(void *data, std::uint32_t id, const spa_pod *param) {
	static_cast< PipeWireScreenVideoSource * >(data)->onStreamParamChanged(id, param);
}

void streamProcessTrampoline(void *data) {
	static_cast< PipeWireScreenVideoSource * >(data)->onStreamProcess();
}

} // namespace

bool PipeWireScreenVideoSource::isAvailable() {
	return PortalScreenCast::isAvailable();
}

PipeWireScreenVideoSource::PipeWireScreenVideoSource(PortalScreenCast::SourceType sourceType, bool captureCursor,
													 QObject *parent)
	: VideoSource(parent), m_sourceType(sourceType), m_captureCursor(captureCursor) {
	ensurePipeWireInitialised();
}

PipeWireScreenVideoSource::~PipeWireScreenVideoSource() {
	stop();
}

QString PipeWireScreenVideoSource::describe() const {
	return m_description.isEmpty() ? tr("Screen") : m_description;
}

bool PipeWireScreenVideoSource::start() {
	if (m_running) {
		return true;
	}

	if (!PortalScreenCast::isAvailable()) {
		emit failed(tr("No desktop portal is available, so the screen cannot be shared."));

		return false;
	}

	m_portal = std::make_unique< PortalScreenCast >(this);

	connect(m_portal.get(), &PortalScreenCast::failed, this, [this](const QString &reason) {
		teardown();
		emit failed(reason);
	});

	// The PipeWire side cannot be built until the portal has produced a node, and the portal is
	// asynchronous because it is showing the user a dialog. start() therefore returns true meaning
	// "the request is under way", exactly as CameraVideoSource does for an asynchronous camera.
	connect(m_portal.get(), &PortalScreenCast::ready, this, [this]() {
		m_description = m_portal->describe();

		m_loop = pw_thread_loop_new("mumble-screencast", nullptr);

		if (!m_loop) {
			teardown();
			emit failed(tr("Could not start the screen capture thread."));

			return;
		}

		pw_thread_loop_lock(m_loop);

		m_context = pw_context_new(pw_thread_loop_get_loop(m_loop), nullptr, 0);

		// The descriptor is handed to PipeWire, which takes ownership of it - the portal object must
		// not close it afterwards, so it is released from there by connecting to this fd directly.
		m_core = m_context ? pw_context_connect_fd(m_context, m_portal->pipeWireFd(), nullptr, 0) : nullptr;

		if (!m_core) {
			pw_thread_loop_unlock(m_loop);
			teardown();
			emit failed(tr("Could not connect to PipeWire for screen capture."));

			return;
		}

		// Zero-initialised and then assigned rather than written as a designated initialiser: the
		// struct has members this code does not use, and naming only some of them is a warning here
		// and a hazard whenever PipeWire adds more.
		static pw_stream_events streamEvents = {};
		streamEvents.version                 = PW_VERSION_STREAM_EVENTS;
		streamEvents.state_changed           = &streamStateChangedTrampoline;
		streamEvents.param_changed           = &streamParamChangedTrampoline;
		streamEvents.process                 = &streamProcessTrampoline;

		m_stream = pw_stream_new(m_core, "mumble-screen-capture",
								 pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
												   PW_KEY_MEDIA_ROLE, "Screen", nullptr));

		if (!m_stream) {
			pw_thread_loop_unlock(m_loop);
			teardown();
			emit failed(tr("Could not create the screen capture stream."));

			return;
		}

		pw_stream_add_listener(m_stream, new spa_hook{}, &streamEvents, this);

		std::uint8_t paramBuffer[1024];
		spa_pod_builder builder = SPA_POD_BUILDER_INIT(paramBuffer, sizeof(paramBuffer));

		// Every packed 32-bit RGB layout a compositor is likely to offer. Narrower would risk a
		// compositor having nothing in common with us; wider would mean writing planar conversions
		// for formats screen capture does not produce.
		// Named because the builder macros take addresses, and the SPA_RECTANGLE/SPA_FRACTION macros
		// produce rvalues.
		const spa_rectangle preferredSize = SPA_RECTANGLE(1920, 1080);
		const spa_rectangle minSize       = SPA_RECTANGLE(1, 1);
		const spa_rectangle maxSize       = SPA_RECTANGLE(MAX_DIMENSION, MAX_DIMENSION);
		const spa_fraction preferredRate  = SPA_FRACTION(30, 1);
		const spa_fraction minRate        = SPA_FRACTION(0, 1);
		const spa_fraction maxRate        = SPA_FRACTION(120, 1);

		const spa_pod *params[1];
		params[0] = static_cast< const spa_pod * >(spa_pod_builder_add_object(
			&builder, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType,
			SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
			SPA_FORMAT_VIDEO_format,
			SPA_POD_CHOICE_ENUM_Id(5, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBA,
								   SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_RGBx),
			SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&preferredSize, &minSize, &maxSize),
			SPA_FORMAT_VIDEO_framerate,
			SPA_POD_CHOICE_RANGE_Fraction(&preferredRate, &minRate, &maxRate)));

		const int connected =
			pw_stream_connect(m_stream, PW_DIRECTION_INPUT, m_portal->nodeId(),
							  static_cast< pw_stream_flags >(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
							  params, 1);

		pw_thread_loop_unlock(m_loop);

		if (connected < 0) {
			teardown();
			emit failed(tr("Could not connect to the shared screen."));

			return;
		}

		if (pw_thread_loop_start(m_loop) < 0) {
			teardown();
			emit failed(tr("Could not start the screen capture thread."));

			return;
		}

		m_clock.restart();
		m_running = true;
	});

	if (!m_portal->requestAccess(m_sourceType, m_captureCursor)) {
		teardown();
		emit failed(tr("Could not ask the desktop portal for permission to share the screen."));

		return false;
	}

	return true;
}

void PipeWireScreenVideoSource::stop() {
	teardown();
}

void PipeWireScreenVideoSource::teardown() {
	m_running = false;

	if (m_loop) {
		// Stopped before anything it might be using is destroyed, so no callback can run against a
		// half-torn-down object.
		pw_thread_loop_stop(m_loop);
	}

	if (m_stream) {
		pw_stream_destroy(m_stream);
		m_stream = nullptr;
	}

	if (m_core) {
		pw_core_disconnect(m_core);
		m_core = nullptr;
	}

	if (m_context) {
		pw_context_destroy(m_context);
		m_context = nullptr;
	}

	if (m_loop) {
		pw_thread_loop_destroy(m_loop);
		m_loop = nullptr;
	}

	m_portal.reset();

	QMutexLocker lock(&m_frameMutex);
	m_pendingFrame    = QImage();
	m_hasPendingFrame = false;
}

void PipeWireScreenVideoSource::onStreamStateChanged(int state, const char *error) {
	if (state != PW_STREAM_STATE_ERROR) {
		return;
	}

	const QString reason = error ? QString::fromUtf8(error) : tr("the screen capture stream stopped");

	// Queued rather than emitted directly: this runs on the PipeWire thread, and failed() handlers tear
	// this object down.
	QMetaObject::invokeMethod(
		this, [this, reason]() { emit failed(tr("Screen sharing stopped: %1").arg(reason)); }, Qt::QueuedConnection);
}

void PipeWireScreenVideoSource::onStreamParamChanged(std::uint32_t id, const spa_pod *param) {
	if (!param || id != SPA_PARAM_Format) {
		return;
	}

	std::uint32_t mediaType    = 0;
	std::uint32_t mediaSubtype = 0;

	if (spa_format_parse(param, &mediaType, &mediaSubtype) < 0 || mediaType != SPA_MEDIA_TYPE_video
		|| mediaSubtype != SPA_MEDIA_SUBTYPE_raw) {
		return;
	}

	spa_video_info_raw info{};

	if (spa_format_video_raw_parse(param, &info) < 0) {
		return;
	}

	const int width  = static_cast< int >(info.size.width);
	const int height = static_cast< int >(info.size.height);

	// The negotiated size comes from outside this process. A compositor will not offer something
	// absurd, but nothing here should depend on that.
	if (width <= 0 || height <= 0 || width > MAX_DIMENSION || height > MAX_DIMENSION) {
		return;
	}

	m_size      = QSize(width, height);
	m_spaFormat = info.format;
}

void PipeWireScreenVideoSource::onStreamProcess() {
	if (!m_stream || !m_size.isValid() || m_spaFormat == 0) {
		return;
	}

	pw_buffer *buffer = pw_stream_dequeue_buffer(m_stream);

	if (!buffer) {
		return;
	}

	spa_buffer *spaBuffer = buffer->buffer;

	// DMA-BUF frames arrive with no mapped pointer. Importing them would need EGL and a GPU context;
	// PW_STREAM_FLAG_MAP_BUFFERS asks for mappable memory instead, so a null pointer here means a
	// buffer this build cannot read rather than one it should try to.
	if (spaBuffer->n_datas < 1 || !spaBuffer->datas[0].data) {
		pw_stream_queue_buffer(m_stream, buffer);

		return;
	}

	const spa_data &data = spaBuffer->datas[0];
	const int width      = m_size.width();
	const int height     = m_size.height();

	// stride is what the producer actually used; it is not necessarily width * 4.
	const std::int32_t stride = data.chunk ? data.chunk->stride : static_cast< std::int32_t >(width) * 4;

	if (stride < static_cast< std::int32_t >(width) * 4
		|| data.maxsize < static_cast< std::uint32_t >(stride) * static_cast< std::uint32_t >(height)) {
		pw_stream_queue_buffer(m_stream, buffer);

		return;
	}

	QImage frame(width, height, QImage::Format_ARGB32);

	const auto *base = static_cast< const std::uint8_t * >(data.data);

	for (int y = 0; y < height; ++y) {
		convertRow(m_spaFormat, base + static_cast< std::size_t >(y) * static_cast< std::size_t >(stride),
				   reinterpret_cast< QRgb * >(frame.scanLine(y)), width);
	}

	pw_stream_queue_buffer(m_stream, buffer);

	publishFrame(frame, static_cast< std::uint64_t >(m_clock.elapsed().count()));
}

void PipeWireScreenVideoSource::publishFrame(const QImage &frame, std::uint64_t captureTimestampUsec) {
	{
		QMutexLocker lock(&m_frameMutex);

		// Replaced rather than queued: only the newest frame is worth having, and a queue would build
		// latency whenever the encoder is slower than the compositor.
		m_pendingFrame         = frame;
		m_pendingTimestampUsec = captureTimestampUsec;
		m_hasPendingFrame      = true;
	}

	// Hops to the owning thread, so frameReady - and everything the broadcaster does in response - runs
	// where the rest of the client does, not on a PipeWire callback.
	QMetaObject::invokeMethod(this, [this]() { deliverPendingFrame(); }, Qt::QueuedConnection);
}

void PipeWireScreenVideoSource::deliverPendingFrame() {
	QImage frame;
	std::uint64_t timestamp = 0;

	{
		QMutexLocker lock(&m_frameMutex);

		if (!m_hasPendingFrame) {
			return;
		}

		frame             = m_pendingFrame;
		timestamp         = m_pendingTimestampUsec;
		m_hasPendingFrame = false;
	}

	if (!frame.isNull()) {
		emit frameReady(frame, timestamp);
	}
}
