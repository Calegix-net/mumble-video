// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "X11ScreenVideoSource.h"

#include <QtGui/QGuiApplication>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>

#include <sys/ipc.h>
#include <sys/shm.h>

#include <algorithm>
#include <utility>

// DefaultRootWindow, DefaultVisual, DefaultDepth and AllPlanes are Xlib macros that reach into the
// Display struct with old-style casts, so the project's warning set rejects them on sight. Suppressed
// for this file only, the same treatment and for the same reason as GlobalShortcut_unix.cpp.
#if defined(__GNUC__)
#	pragma GCC diagnostic ignored "-Wold-style-cast"
#endif

struct X11ScreenVideoSource::Impl {
	Display *display     = nullptr;
	Window root          = 0;
	XImage *shmImage     = nullptr;
	XShmSegmentInfo shm  = {};
	bool shmAttached     = false;
	bool shmSegmentValid = false;
};

bool X11ScreenVideoSource::isAvailable() {
	// A Wayland session is deliberately not offered a direct grab - see the class comment. Asked of Qt
	// rather than of the environment: WAYLAND_DISPLAY may be set in a session whose Qt platform is xcb,
	// and it is the platform Mumble is actually running on that decides what a root-window grab sees.
	if (QGuiApplication::platformName().compare(QLatin1String("wayland"), Qt::CaseInsensitive) == 0) {
		return false;
	}

	// Opening the display is the only honest test. DISPLAY being set says nothing about whether anything
	// is listening on it, and a refused connection must read as "no screen capture here" rather than as
	// a failure at share time.
	Display *display = XOpenDisplay(nullptr);

	if (!display) {
		return false;
	}

	XCloseDisplay(display);

	return true;
}

X11ScreenVideoSource::X11ScreenVideoSource(const QRect &region, QString label, int intervalMsec, QObject *parent)
	: VideoSource(parent), m_impl(std::make_unique< Impl >()), m_region(region), m_label(std::move(label)),
	  m_intervalMsec(std::max(1, intervalMsec)) {
	m_timer.setTimerType(Qt::PreciseTimer);
	connect(&m_timer, &QTimer::timeout, this, &X11ScreenVideoSource::capture);
}

X11ScreenVideoSource::~X11ScreenVideoSource() {
	teardown();
}

QString X11ScreenVideoSource::describe() const {
	return m_label.isEmpty() ? QStringLiteral("X11 screen %1x%2").arg(m_region.width()).arg(m_region.height())
							 : m_label;
}

bool X11ScreenVideoSource::start() {
	if (m_running) {
		return true;
	}

	if (m_region.width() <= 0 || m_region.height() <= 0 || m_region.width() > MAX_DIMENSION
		|| m_region.height() > MAX_DIMENSION) {
		return false;
	}

	m_impl->display = XOpenDisplay(nullptr);

	if (!m_impl->display) {
		return false;
	}

	m_impl->root = DefaultRootWindow(m_impl->display);

	// The visual has to be one whose pixels can be handed to QImage as-is. Everything current is 24 or
	// 32 bit depth with 32 bits per pixel; a 16-bit server would need a conversion path that has no
	// users left, so it is refused here with the reason rather than producing wrong colours.
	const int depth = DefaultDepth(m_impl->display, DefaultScreen(m_impl->display));

	if (depth != 24 && depth != 32) {
		teardown();

		return false;
	}

	// MIT-SHM if the server has it: the image lands in memory shared with the X server rather than being
	// copied through the socket, which for a 1080p frame is ~8 MB per poll. Without it XGetImage still
	// works - notably over a forwarded connection, where shared memory cannot - just more expensively,
	// so a missing extension degrades rather than fails.
	if (XShmQueryExtension(m_impl->display)) {
		m_impl->shmImage = XShmCreateImage(m_impl->display, DefaultVisual(m_impl->display, DefaultScreen(m_impl->display)),
										   static_cast< unsigned int >(depth), ZPixmap, nullptr, &m_impl->shm,
										   static_cast< unsigned int >(m_region.width()),
										   static_cast< unsigned int >(m_region.height()));

		if (m_impl->shmImage) {
			const std::size_t bytes =
				static_cast< std::size_t >(m_impl->shmImage->bytes_per_line) * static_cast< std::size_t >(m_region.height());

			m_impl->shm.shmid = shmget(IPC_PRIVATE, bytes, IPC_CREAT | 0600);

			if (m_impl->shm.shmid != -1) {
				m_impl->shm.shmaddr = m_impl->shmImage->data = static_cast< char * >(shmat(m_impl->shm.shmid, nullptr, 0));
				m_impl->shm.readOnly = False;

				// Marked for destruction immediately: the segment survives until both this process and
				// the X server have detached, so the kernel reclaims it even if we are killed, and a
				// crash cannot leak a segment per share.
				shmctl(m_impl->shm.shmid, IPC_RMID, nullptr);
				m_impl->shmSegmentValid = m_impl->shm.shmaddr != reinterpret_cast< char * >(-1);

				if (m_impl->shmSegmentValid && XShmAttach(m_impl->display, &m_impl->shm)) {
					XSync(m_impl->display, False);
					m_impl->shmAttached = true;
				}
			}
		}

		if (!m_impl->shmAttached && m_impl->shmImage) {
			// data points into the shared segment, which is detached separately below. XDestroyImage
			// would hand a shmat() pointer to free().
			m_impl->shmImage->data = nullptr;
			XDestroyImage(m_impl->shmImage);
			m_impl->shmImage = nullptr;

			if (m_impl->shmSegmentValid) {
				shmdt(m_impl->shm.shmaddr);
				m_impl->shmSegmentValid = false;
			}
		}
	}

	m_previous = QImage();
	m_running  = true;
	m_timer.start(m_intervalMsec);

	return true;
}

void X11ScreenVideoSource::stop() {
	teardown();
}

void X11ScreenVideoSource::teardown() {
	m_timer.stop();
	m_running = false;
	m_previous = QImage();

	if (!m_impl || !m_impl->display) {
		return;
	}

	if (m_impl->shmAttached) {
		XShmDetach(m_impl->display, &m_impl->shm);
		m_impl->shmAttached = false;
	}

	if (m_impl->shmImage) {
		// Frees the XImage but not shmaddr, which is ours: XDestroyImage would call free() on a pointer
		// that came from shmat().
		m_impl->shmImage->data = nullptr;
		XDestroyImage(m_impl->shmImage);
		m_impl->shmImage = nullptr;
	}

	if (m_impl->shmSegmentValid) {
		shmdt(m_impl->shm.shmaddr);
		m_impl->shmSegmentValid = false;
	}

	XCloseDisplay(m_impl->display);
	m_impl->display = nullptr;
}

void X11ScreenVideoSource::capture() {
	if (!m_running || !m_impl->display) {
		return;
	}

	// The root window's size is checked every poll rather than trusted from start(): unplugging a
	// monitor or changing the resolution shrinks it, and a grab reaching past the edge is a BadMatch -
	// which, with no error handler installed, Xlib turns into an exit() of the whole client. Failing
	// with a reason here is the difference between "the share stopped" and "Mumble vanished".
	Window rootReturn;
	int rootX, rootY;
	unsigned int rootWidth = 0, rootHeight = 0, border = 0, rootDepth = 0;

	if (!XGetGeometry(m_impl->display, m_impl->root, &rootReturn, &rootX, &rootY, &rootWidth, &rootHeight, &border,
					  &rootDepth)
		|| m_region.x() < 0 || m_region.y() < 0 || m_region.right() >= static_cast< int >(rootWidth)
		|| m_region.bottom() >= static_cast< int >(rootHeight)) {
		const QString reason = tr("The screen being shared is no longer there (the display layout changed).");

		teardown();
		emit failed(reason);

		return;
	}

	const auto timestamp = static_cast< std::uint64_t >(m_clock.elapsed().count());

	QImage frame;

	if (m_impl->shmAttached) {
		if (!XShmGetImage(m_impl->display, m_impl->root, m_impl->shmImage, m_region.x(), m_region.y(), AllPlanes)) {
			const QString reason = tr("The X server refused to hand over the screen contents.");

			teardown();
			emit failed(reason);

			return;
		}

		// Copied out of the shared segment: the next poll writes into the same memory, and a QImage that
		// merely wraps it would be silently rewritten under whoever is still holding it.
		frame = QImage(reinterpret_cast< const uchar * >(m_impl->shmImage->data), m_region.width(), m_region.height(),
					   m_impl->shmImage->bytes_per_line, QImage::Format_RGB32)
					.copy();
	} else {
		XImage *image = XGetImage(m_impl->display, m_impl->root, m_region.x(), m_region.y(),
								  static_cast< unsigned int >(m_region.width()),
								  static_cast< unsigned int >(m_region.height()), AllPlanes, ZPixmap);

		if (!image) {
			const QString reason = tr("The X server refused to hand over the screen contents.");

			teardown();
			emit failed(reason);

			return;
		}

		if (image->bits_per_pixel == 32) {
			frame = QImage(reinterpret_cast< const uchar * >(image->data), m_region.width(), m_region.height(),
						   image->bytes_per_line, QImage::Format_RGB32)
						.copy();
		}

		XDestroyImage(image);

		if (frame.isNull()) {
			const QString reason = tr("This X server's pixel format is not one this client can read.");

			teardown();
			emit failed(reason);

			return;
		}
	}

	// A screen that has not changed is the normal case, and the contract asks for captureIdle rather
	// than a frame. The comparison is over the raw RGB32 words, so an X server that leaves junk in the
	// unused top byte can make an unchanged screen look changed - which costs one encode that finds no
	// changed tiles, not a wrong picture.
	if (!m_previous.isNull() && m_previous == frame) {
		emit captureIdle(timestamp);

		return;
	}

	m_previous = frame;

	emit frameReady(frame, timestamp);
}
