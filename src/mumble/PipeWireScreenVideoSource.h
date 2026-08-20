// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_PIPEWIRESCREENVIDEOSOURCE_H_
#define MUMBLE_MUMBLE_PIPEWIRESCREENVIDEOSOURCE_H_

#include "PortalScreenCast.h"
#include "Timer.h"
#include "VideoSource.h"

#include <QtCore/QMutex>
#include <QtCore/QSize>
#include <QtGui/QImage>

#include <atomic>
#include <cstdint>
#include <memory>

// Deliberately no PipeWire headers here. Every file that includes this one would otherwise pull them
// in - MainWindow.cpp among them - and PipeWire's headers do not survive being compiled inside this
// project's unity blobs: SPA macros they depend on go missing even though the same includes compile
// cleanly on their own. Forward declarations are enough for the members below, and the callbacks that
// genuinely need PipeWire types live entirely in the .cpp.
struct pw_thread_loop;
struct pw_context;
struct pw_core;
struct pw_stream;

/**
 * Captures the screen on Linux: permission from the XDG desktop portal, frames from PipeWire.
 *
 * This is the counterpart of DxgiDisplayVideoSource and WgcWindowVideoSource. It covers both of their
 * jobs, because the choice between a display and a window is made in the portal's own picker rather
 * than in ours - the application is told which PipeWire node to read, and nothing about what the node
 * is showing changes how it is read.
 *
 * Works on Wayland, where nothing else can: the compositor will not let a client read the screen, and
 * the portal exists precisely to mediate that. It works on X11 too, through the same path, which is
 * deliberate - an XGetImage fallback would capture without the consent step and there is no good
 * reason to have one.
 *
 * PipeWire runs its own thread. Buffers arrive on it, are converted to QImage there, and are handed
 * over under a mutex; frameReady is emitted on the owning thread by a zero-interval timer, so nothing
 * downstream of VideoSource ever runs on a PipeWire callback.
 */
class PipeWireScreenVideoSource : public VideoSource {
	Q_OBJECT

public:
	/// Whether this platform can capture the screen at all - true when a desktop portal is answering.
	static bool isAvailable();

	/**
	 * @param sourceType What to ask the portal to offer. The user still chooses within that.
	 * @param captureCursor Whether the pointer is drawn into the frames.
	 */
	explicit PipeWireScreenVideoSource(PortalScreenCast::SourceType sourceType, bool captureCursor,
									   QObject *parent = nullptr);
	~PipeWireScreenVideoSource() override;

	bool start() override;
	void stop() override;
	bool isRunning() const override { return m_running; }
	QString describe() const override;

	/// Largest frame accepted, per side. A capture source is fed by the compositor rather than by a
	/// peer, but a negotiated format is still external input and an 8K monitor would allocate ~130 MB
	/// per frame at 4 bytes a pixel.
	static constexpr int MAX_DIMENSION = 8192;

	// Called from the PipeWire thread, by file-static trampolines in the .cpp - public because those
	// are free functions, not members.
	void onStreamParamChanged(std::uint32_t id, const struct spa_pod *param);
	void onStreamProcess();
	/// @param state A pw_stream_state, passed as int so this header needs no PipeWire types.
	void onStreamStateChanged(int state, const char *error);

protected:
	/// Hands a converted frame to the Qt side. Called on the PipeWire thread.
	void publishFrame(const QImage &frame, std::uint64_t captureTimestampUsec);

	/// Drains the pending frame on the owning thread and emits frameReady.
	void deliverPendingFrame();

	void teardown();

	std::unique_ptr< PortalScreenCast > m_portal;

	PortalScreenCast::SourceType m_sourceType;
	bool m_captureCursor;

	pw_thread_loop *m_loop = nullptr;
	pw_context *m_context  = nullptr;
	pw_core *m_core        = nullptr;
	pw_stream *m_stream    = nullptr;

	/// Negotiated format. Written on the PipeWire thread during param negotiation, read there too.
	QSize m_size;
	std::uint32_t m_spaFormat = 0;

	std::atomic< bool > m_running{ false };

	/// Most recent frame, waiting to be picked up by the owning thread. Only the newest is kept: this
	/// is live video, and a backlog would show the viewer the past rather than the present.
	QMutex m_frameMutex;
	QImage m_pendingFrame;
	std::uint64_t m_pendingTimestampUsec = 0;
	bool m_hasPendingFrame               = false;

	Timer m_clock;

	QString m_description;
};

#endif // MUMBLE_MUMBLE_PIPEWIRESCREENVIDEOSOURCE_H_
