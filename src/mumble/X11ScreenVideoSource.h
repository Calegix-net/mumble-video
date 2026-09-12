// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_X11SCREENVIDEOSOURCE_H_
#define MUMBLE_MUMBLE_X11SCREENVIDEOSOURCE_H_

#include "Timer.h"
#include "VideoSource.h"

#include <QtCore/QRect>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtGui/QImage>

#include <memory>

/**
 * Captures a region of an X11 screen directly, with the MIT-SHM extension where the server offers it.
 *
 * The fallback for X11 desktops that have no ScreenCast portal. That is not an exotic case: the only
 * portal backend on XFCE, MATE, Cinnamon and most tiling window managers is xdg-desktop-portal-gtk,
 * which implements ten portals and not ScreenCast, so PipeWireScreenVideoSource::isAvailable() answers
 * false and - before this existed - the Share Screen button simply was not there, with nothing to tell
 * the user why.
 *
 * PipeWire through the portal stays the preferred path wherever it answers, and this is only reached
 * when it does not. Two reasons, in order of importance:
 *
 *  1. Under Wayland the portal is the only thing that can capture at all, and what an X11 grab would
 *     see there is the XWayland root window - the X clients only, on an otherwise empty desktop, which
 *     is worse than useless because it looks like a working share of the wrong thing. isAvailable()
 *     therefore refuses on a Wayland session rather than offering that.
 *  2. The portal's picker is the desktop's own consent step. A direct grab has none, which is why this
 *     class takes an explicit region: the caller asks the user which screen to share and passes the
 *     answer in. On X11 that is a choice-of-target question rather than a privacy one - any client can
 *     already read the root window, the protocol has no capture isolation - but a share that silently
 *     grabs every monitor because nobody was asked is still the wrong behaviour.
 *
 * Frames are polled on a timer on the owning thread. Xlib is only ever touched from there, so this
 * needs no XInitThreads and shares no state with the Xlib connection GlobalShortcutX keeps.
 */
class X11ScreenVideoSource : public VideoSource {
	Q_OBJECT

public:
	/// Whether a direct X11 grab is possible and sensible here: an X server that accepts a connection,
	/// and a session that is not Wayland (see the class comment for why Wayland is excluded).
	static bool isAvailable();

	/**
	 * @param region Area of the X root window to capture, in X pixels.
	 * @param label How to describe this capture in the UI and in logs, e.g. the monitor's name.
	 * @param intervalMsec Poll interval. The caller derives it from the configured frame rate.
	 */
	X11ScreenVideoSource(const QRect &region, QString label, int intervalMsec, QObject *parent = nullptr);
	~X11ScreenVideoSource() override;

	bool start() override;
	void stop() override;
	bool isRunning() const override { return m_running; }
	QString describe() const override;

	/// Largest region accepted, per side. Matches PipeWireScreenVideoSource: an 8K frame is already
	/// ~130 MB at four bytes a pixel, and a region larger than that is a bug rather than a desktop.
	static constexpr int MAX_DIMENSION = 8192;

private:
	void capture();
	void teardown();

	/// Xlib and MIT-SHM state. Behind a pointer so that X11/Xlib.h stays in the .cpp: it defines None,
	/// Status and KeyPress as macros, which collide with Qt and with this project's own names in every
	/// translation unit that would otherwise pull them in - GlobalShortcut_unix.h has to #undef four of
	/// them for exactly that reason.
	struct Impl;
	std::unique_ptr< Impl > m_impl;

	QRect m_region;
	QString m_label;
	int m_intervalMsec;
	bool m_running = false;

	/// The previous frame, to answer the captureIdle half of the VideoSource contract. A screen that is
	/// not changing is the normal case, and saying so is cheaper for everything downstream than handing
	/// the encoder an identical frame to diff tile by tile.
	QImage m_previous;

	QTimer m_timer;
	Timer m_clock;
};

#endif
