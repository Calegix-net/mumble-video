// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "ScreenAwake.h"

#include <QtCore/QDebug>

#ifdef Q_OS_WIN
#	include "win.h"
#elif defined(USE_DBUS)
#	include <QtCore/QCoreApplication>
#	include <QtCore/QVariantMap>
#	include <QtDBus/QDBusConnection>
#	include <QtDBus/QDBusMessage>
#	include <QtDBus/QDBusObjectPath>
#	include <QtDBus/QDBusPendingCallWatcher>
#	include <QtDBus/QDBusPendingReply>
#endif

namespace {
#ifdef USE_DBUS
const QString PORTAL_SERVICE      = QStringLiteral("org.freedesktop.portal.Desktop");
const QString PORTAL_PATH         = QStringLiteral("/org/freedesktop/portal/desktop");
const QString PORTAL_INHIBIT      = QStringLiteral("org.freedesktop.portal.Inhibit");
const QString PORTAL_REQUEST      = QStringLiteral("org.freedesktop.portal.Request");
const QString SCREENSAVER_SERVICE = QStringLiteral("org.freedesktop.ScreenSaver");
const QString SCREENSAVER_PATH    = QStringLiteral("/org/freedesktop/ScreenSaver");
/// The portal's flag for "the session going idle", which is what blanks and locks the screen.
constexpr unsigned int PORTAL_INHIBIT_IDLE = 8;
/// Short, because quitting waits for a call still in flight.
constexpr int CALL_TIMEOUT_MSEC = 5000;

QString reason() {
	return ScreenAwake::tr("Video is being shared or watched");
}
#endif
} // namespace

bool ScreenAwake::wanted(int mode, bool sendingVideo, bool watchingVideo) {
	switch (mode) {
		case WhileVideo:
			return sendingVideo || watchingVideo;
		case WhileRunning:
			return true;
		default:
			return false;
	}
}

ScreenAwake::ScreenAwake(QObject *parent) : QObject(parent) {
}

ScreenAwake::~ScreenAwake() {
	m_wanted = false;
#ifdef Q_OS_WIN
	apply();
#elif defined(USE_DBUS)
	// Waited for, so that quitting does not leave an inhibition behind for the moment it takes the portal
	// to notice this process has gone. A call still in flight is finished first so that what it was
	// granted is closed too.
	m_closing = true;
	for (int i = 0; m_call && i < 2; ++i) {
		m_call->waitForFinished();
	}
	release(true);
#endif
}

void ScreenAwake::setInhibited(bool inhibited) {
	if (inhibited == m_wanted) {
		return;
	}
	m_wanted = inhibited;
	apply();
}

#ifdef Q_OS_WIN

bool ScreenAwake::isHeld() const {
	return m_held;
}

void ScreenAwake::apply() {
	if (m_wanted == m_held) {
		return;
	}
	// Per thread, and this object lives on the GUI thread, so the request is held for as long as the
	// client runs or until it is cleared here.
	const EXECUTION_STATE state =
		m_wanted ? (ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED) : ES_CONTINUOUS;
	if (SetThreadExecutionState(state) == 0) {
		qWarning("ScreenAwake: SetThreadExecutionState failed");
		return;
	}
	m_held = m_wanted;
}

#elif defined(USE_DBUS)

bool ScreenAwake::isHeld() const {
	return !m_portalHandle.isEmpty() || m_haveCookie;
}

void ScreenAwake::apply() {
	if (m_pending) {
		return;
	}
	if (m_wanted && !isHeld()) {
		inhibitPortal();
	} else if (!m_wanted && isHeld()) {
		release(m_closing);
	}
}

void ScreenAwake::inhibitPortal() {
	QDBusMessage call = QDBusMessage::createMethodCall(PORTAL_SERVICE, PORTAL_PATH, PORTAL_INHIBIT,
													   QStringLiteral("Inhibit"));
	QVariantMap options;
	options.insert(QStringLiteral("reason"), reason());
	call << QString() << PORTAL_INHIBIT_IDLE << options;

	m_pending = true;
	m_call    = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call, CALL_TIMEOUT_MSEC), this);
	connect(m_call, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
		w->deleteLater();
		m_call                                           = nullptr;
		const QDBusPendingReply< QDBusObjectPath > reply = *w;
		if (reply.isError() && !m_wanted) {
			m_pending = false;
			return;
		}
		if (reply.isError()) {
			qInfo() << "ScreenAwake: no inhibit portal, trying org.freedesktop.ScreenSaver:" << reply.error().message();
			inhibitScreenSaver();
			return;
		}
		m_pending      = false;
		m_portalHandle = reply.value().path();
		apply();
	});
}

void ScreenAwake::inhibitScreenSaver() {
	QDBusMessage call = QDBusMessage::createMethodCall(SCREENSAVER_SERVICE, SCREENSAVER_PATH, SCREENSAVER_SERVICE,
													   QStringLiteral("Inhibit"));
	call << QCoreApplication::applicationName() << reason();

	m_call = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call, CALL_TIMEOUT_MSEC), this);
	connect(m_call, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
		w->deleteLater();
		m_call                                = nullptr;
		m_pending                             = false;
		const QDBusPendingReply< uint > reply = *w;
		if (reply.isError()) {
			// Not retried until the request changes: without either service there is nothing to ask.
			qWarning() << "ScreenAwake: cannot keep the screen awake:" << reply.error().message();
			return;
		}
		m_cookie     = reply.value();
		m_haveCookie = true;
		apply();
	});
}

void ScreenAwake::release(bool wait) {
	QDBusConnection bus = QDBusConnection::sessionBus();
	const auto send     = [&bus, wait](const QDBusMessage &message) {
        if (wait) {
            bus.call(message, QDBus::Block, CALL_TIMEOUT_MSEC);
        } else {
            bus.send(message);
        }
	};
	if (!m_portalHandle.isEmpty()) {
		send(QDBusMessage::createMethodCall(PORTAL_SERVICE, m_portalHandle, PORTAL_REQUEST, QStringLiteral("Close")));
		m_portalHandle.clear();
	}
	if (m_haveCookie) {
		QDBusMessage call = QDBusMessage::createMethodCall(SCREENSAVER_SERVICE, SCREENSAVER_PATH, SCREENSAVER_SERVICE,
														   QStringLiteral("UnInhibit"));
		call << m_cookie;
		send(call);
		m_haveCookie = false;
	}
}

#else

bool ScreenAwake::isHeld() const {
	return false;
}

void ScreenAwake::apply() {
}

#endif
