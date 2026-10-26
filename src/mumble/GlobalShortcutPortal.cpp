// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "GlobalShortcutPortal.h"

#include "EnvUtils.h"

#include <QtCore/QRandomGenerator>
#include <QtDBus/QDBusMessage>
#include <QtDBus/QDBusMetaType>
#include <QtDBus/QDBusPendingCallWatcher>
#include <QtDBus/QDBusPendingReply>

namespace {

const char *PORTAL_SERVICE   = "org.freedesktop.portal.Desktop";
const char *PORTAL_PATH      = "/org/freedesktop/portal/desktop";
const char *SHORTCUTS_IFACE  = "org.freedesktop.portal.GlobalShortcuts";
const char *REQUEST_IFACE    = "org.freedesktop.portal.Request";
const char *SESSION_IFACE    = "org.freedesktop.portal.Session";

constexpr uint PORTAL_RESPONSE_SUCCESS = 0;

} // namespace

QDBusArgument &operator<<(QDBusArgument &argument, const PortalShortcut &shortcut) {
	argument.beginStructure();
	argument << shortcut.id << shortcut.properties;
	argument.endStructure();
	return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, PortalShortcut &shortcut) {
	argument.beginStructure();
	argument >> shortcut.id >> shortcut.properties;
	argument.endStructure();
	return argument;
}

GlobalShortcutPortal::GlobalShortcutPortal(QObject *parent)
	: GlobalShortcutPortal(QDBusConnection::sessionBus(), parent) {
}

GlobalShortcutPortal::GlobalShortcutPortal(const QDBusConnection &bus, QObject *parent) : QObject(parent), m_bus(bus) {
	init();
}

GlobalShortcutPortal::~GlobalShortcutPortal() {
	stop();
}

void GlobalShortcutPortal::init() {
	qDBusRegisterMetaType< PortalShortcut >();
	qDBusRegisterMetaType< PortalShortcutList >();
}

bool GlobalShortcutPortal::appliesToThisSession() {
	return EnvUtils::waylandIsUsed() && QDBusConnection::sessionBus().isConnected();
}

QString GlobalShortcutPortal::newToken() {
	return QStringLiteral("mumble%1").arg(QRandomGenerator::global()->generate(), 0, 16);
}

QString GlobalShortcutPortal::senderPathElement() const {
	// The documented transformation of the caller's unique name: the leading ':' dropped and every '.'
	// turned into '_'.
	QString sender = m_bus.baseService();

	if (sender.startsWith(QLatin1Char(':'))) {
		sender.remove(0, 1);
	}

	return sender.replace(QLatin1Char('.'), QLatin1Char('_'));
}

void GlobalShortcutPortal::start(const PortalShortcutList &shortcuts) {
	m_offered = shortcuts;

	if (m_state != State::Idle && m_state != State::Failed) {
		return;
	}

	if (!m_bus.isConnected()) {
		fail(QStringLiteral("no session bus"));
		return;
	}

	m_state = State::Creating;

	const QString sessionToken = newToken();
	QVariantMap options;
	options.insert(QStringLiteral("session_handle_token"), sessionToken);
	m_sessionHandle =
		QStringLiteral("/org/freedesktop/portal/desktop/session/%1/%2").arg(senderPathElement(), sessionToken);

	if (!callWithRequest(QStringLiteral("CreateSession"), {}, options,
						 SLOT(onCreateSessionResponse(uint, QVariantMap)))) {
		fail(QStringLiteral("CreateSession could not be sent"));
	}
}

void GlobalShortcutPortal::setShortcuts(const PortalShortcutList &shortcuts) {
	if (shortcuts == m_offered) {
		return;
	}

	m_offered = shortcuts;

	switch (m_state) {
		case State::Bound:
			bind();
			break;
		case State::Creating:
		case State::Binding:
			// The answer to the request in flight describes the old set; bind again once it is in.
			m_rebindPending = true;
			break;
		case State::Idle:
		case State::Failed:
			break;
	}
}

void GlobalShortcutPortal::configure() {
	if (m_state != State::Bound) {
		return;
	}

	QDBusMessage call = QDBusMessage::createMethodCall(QLatin1String(PORTAL_SERVICE), QLatin1String(PORTAL_PATH),
													   QLatin1String(SHORTCUTS_IFACE), QStringLiteral("ConfigureShortcuts"));
	call << QVariant::fromValue(QDBusObjectPath(m_sessionHandle)) << QString() << QVariantMap();

	auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(call), this);
	connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *self) {
		self->deleteLater();
		if (self->isError()) {
			// ConfigureShortcuts only exists from version 2 of the interface. Binding again is what makes
			// a version 1 portal show its dialog.
			bind();
		}
	});
}

void GlobalShortcutPortal::stop() {
	dropRequest();

	if (!m_sessionHandle.isEmpty() && (m_state == State::Bound || m_state == State::Binding)) {
		m_bus.disconnect(QLatin1String(PORTAL_SERVICE), QLatin1String(PORTAL_PATH), QLatin1String(SHORTCUTS_IFACE),
						 QStringLiteral("Activated"), this,
						 SLOT(onActivated(QDBusObjectPath, QString, qulonglong, QVariantMap)));
		m_bus.disconnect(QLatin1String(PORTAL_SERVICE), QLatin1String(PORTAL_PATH), QLatin1String(SHORTCUTS_IFACE),
						 QStringLiteral("Deactivated"), this,
						 SLOT(onDeactivated(QDBusObjectPath, QString, qulonglong, QVariantMap)));
		m_bus.disconnect(QLatin1String(PORTAL_SERVICE), QLatin1String(PORTAL_PATH), QLatin1String(SHORTCUTS_IFACE),
						 QStringLiteral("ShortcutsChanged"), this,
						 SLOT(onShortcutsChanged(QDBusObjectPath, PortalShortcutList)));

		QDBusMessage close = QDBusMessage::createMethodCall(QLatin1String(PORTAL_SERVICE), m_sessionHandle,
															QLatin1String(SESSION_IFACE), QStringLiteral("Close"));
		m_bus.asyncCall(close);
	}

	m_state         = State::Idle;
	m_rebindPending = false;
	m_assigned.clear();
}

QString GlobalShortcutPortal::triggerDescription(const QString &id) const {
	for (const PortalShortcut &shortcut : m_assigned) {
		if (shortcut.id == id) {
			return shortcut.properties.value(QStringLiteral("trigger_description")).toString();
		}
	}

	return QString();
}

bool GlobalShortcutPortal::callWithRequest(const QString &method, QList< QVariant > arguments, QVariantMap options,
										   const char *slot) {
	dropRequest();

	const QString token = newToken();
	options.insert(QStringLiteral("handle_token"), token);
	arguments.append(options);

	// Subscribed before the call, at the documented path: the Response can be emitted before the method
	// reply is even sent, and a subscription made after that reply would already have missed it.
	m_requestPath = QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(senderPathElement(), token);
	if (!m_bus.connect(QLatin1String(PORTAL_SERVICE), m_requestPath, QLatin1String(REQUEST_IFACE),
					   QStringLiteral("Response"), this, slot)) {
		m_requestPath.clear();
		return false;
	}

	QDBusMessage call = QDBusMessage::createMethodCall(QLatin1String(PORTAL_SERVICE), QLatin1String(PORTAL_PATH),
													   QLatin1String(SHORTCUTS_IFACE), method);
	call.setArguments(arguments);

	const QString expected = m_requestPath;
	auto *watcher          = new QDBusPendingCallWatcher(m_bus.asyncCall(call), this);
	connect(watcher, &QDBusPendingCallWatcher::finished, this,
			[this, expected, method, slot](QDBusPendingCallWatcher *self) {
				self->deleteLater();

				// Superseded by a later request, or stopped: nothing here is still waiting for this one.
				if (m_requestPath != expected) {
					return;
				}

				const QDBusPendingReply< QDBusObjectPath > reply = *self;
				if (reply.isError()) {
					// No portal, or a portal without GlobalShortcuts (UnknownMethod / UnknownInterface).
					fail(QStringLiteral("%1 failed: %2").arg(method, reply.error().message()));
					return;
				}

				// An older portal may answer on a path other than the predicted one; the reply is
				// authoritative.
				const QString actual = reply.value().path();
				if (!actual.isEmpty() && actual != expected) {
					m_bus.disconnect(QLatin1String(PORTAL_SERVICE), expected, QLatin1String(REQUEST_IFACE),
									 QStringLiteral("Response"), this, slot);
					m_requestPath = actual;
					m_bus.connect(QLatin1String(PORTAL_SERVICE), actual, QLatin1String(REQUEST_IFACE),
								  QStringLiteral("Response"), this, slot);
				}
			});

	return true;
}

void GlobalShortcutPortal::dropRequest() {
	if (m_requestPath.isEmpty()) {
		return;
	}

	// Whichever slot it was subscribed with; disconnecting one that is not connected is harmless.
	for (const char *slot :
		 { SLOT(onCreateSessionResponse(uint, QVariantMap)), SLOT(onBindShortcutsResponse(uint, QVariantMap)) }) {
		m_bus.disconnect(QLatin1String(PORTAL_SERVICE), m_requestPath, QLatin1String(REQUEST_IFACE),
						 QStringLiteral("Response"), this, slot);
	}

	m_requestPath.clear();
}

void GlobalShortcutPortal::onCreateSessionResponse(uint response, const QVariantMap &results) {
	if (m_state != State::Creating) {
		return;
	}
	dropRequest();

	if (response != PORTAL_RESPONSE_SUCCESS) {
		fail(QStringLiteral("the portal refused to create a session (response %1)").arg(response));
		return;
	}

	// An object path in current xdg-desktop-portal, a plain string in older ones.
	const QVariant handle = results.value(QStringLiteral("session_handle"));
	QString path;
	if (handle.canConvert< QDBusObjectPath >()) {
		path = handle.value< QDBusObjectPath >().path();
	}
	if (path.isEmpty()) {
		path = handle.toString();
	}
	if (!path.isEmpty()) {
		m_sessionHandle = path;
	}

	m_bus.connect(QLatin1String(PORTAL_SERVICE), QLatin1String(PORTAL_PATH), QLatin1String(SHORTCUTS_IFACE),
				  QStringLiteral("Activated"), this, SLOT(onActivated(QDBusObjectPath, QString, qulonglong, QVariantMap)));
	m_bus.connect(QLatin1String(PORTAL_SERVICE), QLatin1String(PORTAL_PATH), QLatin1String(SHORTCUTS_IFACE),
				  QStringLiteral("Deactivated"), this,
				  SLOT(onDeactivated(QDBusObjectPath, QString, qulonglong, QVariantMap)));
	m_bus.connect(QLatin1String(PORTAL_SERVICE), QLatin1String(PORTAL_PATH), QLatin1String(SHORTCUTS_IFACE),
				  QStringLiteral("ShortcutsChanged"), this,
				  SLOT(onShortcutsChanged(QDBusObjectPath, PortalShortcutList)));

	bind();
}

void GlobalShortcutPortal::bind() {
	m_state         = State::Binding;
	m_rebindPending = false;

	if (!callWithRequest(QStringLiteral("BindShortcuts"),
						 { QVariant::fromValue(QDBusObjectPath(m_sessionHandle)), QVariant::fromValue(m_offered),
						   QString() },
						 {}, SLOT(onBindShortcutsResponse(uint, QVariantMap)))) {
		fail(QStringLiteral("BindShortcuts could not be sent"));
	}
}

void GlobalShortcutPortal::onBindShortcutsResponse(uint response, const QVariantMap &results) {
	if (m_state != State::Binding) {
		return;
	}
	dropRequest();

	// A cancelled dialog still leaves a usable session: whatever the user had assigned before stays
	// assigned, and the desktop's own settings can add the rest later.
	m_state = State::Bound;
	if (response == PORTAL_RESPONSE_SUCCESS) {
		updateAssigned(results.value(QStringLiteral("shortcuts")));
	}

	emit bound();

	if (m_rebindPending) {
		bind();
	}
}

void GlobalShortcutPortal::updateAssigned(const QVariant &shortcuts) {
	PortalShortcutList assigned;

	if (shortcuts.canConvert< QDBusArgument >()) {
		shortcuts.value< QDBusArgument >() >> assigned;
	} else if (shortcuts.canConvert< PortalShortcutList >()) {
		assigned = shortcuts.value< PortalShortcutList >();
	}

	if (assigned != m_assigned) {
		m_assigned = assigned;
		emit shortcutsChanged();
	}
}

void GlobalShortcutPortal::onActivated(const QDBusObjectPath &session, const QString &id, qulonglong, const QVariantMap &) {
	// The signal is broadcast for every session of this connection; only ours counts.
	if (m_state == State::Bound && session.path() == m_sessionHandle) {
		emit activated(id);
	}
}

void GlobalShortcutPortal::onDeactivated(const QDBusObjectPath &session, const QString &id, qulonglong,
										 const QVariantMap &) {
	if (m_state == State::Bound && session.path() == m_sessionHandle) {
		emit deactivated(id);
	}
}

void GlobalShortcutPortal::onShortcutsChanged(const QDBusObjectPath &session, const PortalShortcutList &shortcuts) {
	if (session.path() != m_sessionHandle) {
		return;
	}

	if (shortcuts != m_assigned) {
		m_assigned = shortcuts;
		emit shortcutsChanged();
	}
}

void GlobalShortcutPortal::fail(const QString &reason) {
	dropRequest();
	m_state = State::Failed;
	qWarning("GlobalShortcutPortal: %s", qUtf8Printable(reason));
	emit unavailable(reason);
}
