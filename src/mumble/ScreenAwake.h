// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_SCREENAWAKE_H_
#define MUMBLE_MUMBLE_SCREENAWAKE_H_

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>

class QDBusPendingCallWatcher;

/**
 * Stops the screen saver and display sleep while video is being shared or watched (the "Keep the screen
 * awake" video setting).
 *
 * Windows asks for the display with SetThreadExecutionState. Linux asks the desktop portal's Inhibit
 * interface, which works inside Flatpak, and falls back to org.freedesktop.ScreenSaver where there is no
 * portal. Both D-Bus calls are asynchronous; a change of mind while one is in flight is applied when it
 * answers. The inhibition is lifted when it is no longer wanted and when this object is destroyed.
 */
class ScreenAwake : public QObject {
	Q_OBJECT
	Q_DISABLE_COPY(ScreenAwake)

public:
	/// Settings::iKeepScreenAwake.
	enum Mode { Off = 0, WhileVideo = 1, WhileRunning = 2 };

	/// Whether the screen should be kept awake right now. [sendingVideo]: a camera or screen of ours is
	/// being shared. [watchingVideo]: someone else's stream is being watched in a tile, fullscreen or popped
	/// out.
	static bool wanted(int mode, bool sendingVideo, bool watchingVideo);

	explicit ScreenAwake(QObject *parent = nullptr);
	~ScreenAwake() override;

	/// Asks for, or lifts, the inhibition. Repeating the current request does nothing.
	void setInhibited(bool inhibited);

	/// What was last asked for.
	bool isInhibitedRequested() const { return m_wanted; }

	/// Whether an inhibition is actually held right now.
	bool isHeld() const;

private:
	bool m_wanted = false;

	void apply();

#ifdef Q_OS_WIN
	bool m_held = false;
#elif defined(USE_DBUS)
	bool m_pending = false;
	/// Set while being destroyed, when a release must be sent before the process can go.
	bool m_closing = false;
	/// The Inhibit call in flight, if any.
	QPointer< QDBusPendingCallWatcher > m_call;
	/// The portal's Request object while it holds the inhibition.
	QString m_portalHandle;
	/// org.freedesktop.ScreenSaver's cookie while it holds the inhibition.
	unsigned int m_cookie = 0;
	bool m_haveCookie     = false;

	void inhibitPortal();
	void inhibitScreenSaver();
	void release(bool wait);
#endif
};

#endif // MUMBLE_MUMBLE_SCREENAWAKE_H_
