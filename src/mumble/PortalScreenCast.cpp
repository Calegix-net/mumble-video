// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "PortalScreenCast.h"

#include <QtCore/QRandomGenerator>
#include <QtCore/QVariantMap>
#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusInterface>
#include <QtDBus/QDBusMessage>
#include <QtDBus/QDBusPendingCall>
#include <QtDBus/QDBusReply>
#include <QtDBus/QDBusUnixFileDescriptor>

#include <unistd.h>

namespace {

const char *PORTAL_SERVICE   = "org.freedesktop.portal.Desktop";
const char *PORTAL_PATH      = "/org/freedesktop/portal/desktop";
const char *SCREENCAST_IFACE = "org.freedesktop.portal.ScreenCast";
const char *REQUEST_IFACE    = "org.freedesktop.portal.Request";
const char *SESSION_IFACE    = "org.freedesktop.portal.Session";

/// Portal source-type bitmask, from the ScreenCast interface documentation.
constexpr std::uint32_t PORTAL_SOURCE_MONITOR = 1;
constexpr std::uint32_t PORTAL_SOURCE_WINDOW  = 2;

/// Portal response codes. 0 is success; 1 is the user cancelling, which is not an error worth
/// reporting as a failure the way a genuine fault is.
constexpr std::uint32_t PORTAL_RESPONSE_SUCCESS   = 0;
constexpr std::uint32_t PORTAL_RESPONSE_CANCELLED = 1;

} // namespace

PortalScreenCast::PortalScreenCast(QObject *parent) : QObject(parent) {
	// Tokens must be unique per client and valid as a D-Bus path element, so no dashes or digits at
	// the start.
	m_token = newToken();
}

PortalScreenCast::~PortalScreenCast() {
	close();
}

bool PortalScreenCast::isAvailable() {
	if (!QDBusConnection::sessionBus().isConnected()) {
		return false;
	}

	QDBusInterface iface(QLatin1String(PORTAL_SERVICE), QLatin1String(PORTAL_PATH),
						 QLatin1String(SCREENCAST_IFACE), QDBusConnection::sessionBus());

	// isValid() only means the proxy was constructed. Reading a property is what actually round-trips
	// to the portal and proves an implementation is running and answering.
	return iface.isValid() && iface.property("version").isValid();
}

void PortalScreenCast::fail(const QString &reason) {
	close();

	emit failed(reason);
}

QString PortalScreenCast::newToken() {
	// Unique per request, as the portal documentation asks: a Request object lives at a path derived
	// from the token, and reusing one across CreateSession/SelectSources/Start would have every
	// request's Response arrive at the same object path.
	return QStringLiteral("mumble%1").arg(QRandomGenerator::global()->generate(), 0, 16);
}

QString PortalScreenCast::senderPathElement() {
	// The portal builds Request/Session paths from the caller's unique bus name with the leading ':'
	// dropped and every '.' turned into '_' - the documented transformation.
	QString sender = QDBusConnection::sessionBus().baseService();

	if (sender.startsWith(QLatin1Char(':'))) {
		sender.remove(0, 1);
	}

	return sender.replace(QLatin1Char('.'), QLatin1Char('_'));
}

bool PortalScreenCast::connectRequest(const QDBusObjectPath &path, const char *slot) {
	return QDBusConnection::sessionBus().connect(QLatin1String(PORTAL_SERVICE), path.path(),
												 QLatin1String(REQUEST_IFACE), QStringLiteral("Response"), this,
												 slot);
}

bool PortalScreenCast::callWithRequest(const QString &method, QList< QVariant > arguments, QVariantMap options,
									   const char *slot) {
	QDBusInterface iface(QLatin1String(PORTAL_SERVICE), QLatin1String(PORTAL_PATH),
						 QLatin1String(SCREENCAST_IFACE), QDBusConnection::sessionBus());

	if (!iface.isValid()) {
		return false;
	}

	const QString token = newToken();
	options.insert(QStringLiteral("handle_token"), token);
	arguments.append(options);

	// Subscribed BEFORE the call, at the path the portal is documented to use. The Response signal can
	// be emitted as soon as the portal has the request - for CreateSession, before the method reply is
	// even on its way back - and a subscription made after the reply arrives has already missed it.
	// Missing it looks like the portal never answering, or (with the old single shared token) like a
	// stale response from the previous request landing on the next.
	const QDBusObjectPath expected(QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2")
									   .arg(senderPathElement(), token));

	connectRequest(expected, slot);

	const QDBusReply< QDBusObjectPath > reply = iface.callWithArgumentList(QDBus::Block, method, arguments);

	if (!reply.isValid()) {
		QDBusConnection::sessionBus().disconnect(QLatin1String(PORTAL_SERVICE), expected.path(),
												 QLatin1String(REQUEST_IFACE), QStringLiteral("Response"), this,
												 slot);

		return false;
	}

	// An older portal may hand back a path other than the predicted one; the reply is authoritative.
	if (reply.value().path() != expected.path()) {
		QDBusConnection::sessionBus().disconnect(QLatin1String(PORTAL_SERVICE), expected.path(),
												 QLatin1String(REQUEST_IFACE), QStringLiteral("Response"), this,
												 slot);

		return connectRequest(reply.value(), slot);
	}

	return true;
}

bool PortalScreenCast::requestAccess(SourceType sourceType, bool allowCursor) {
	m_sourceType  = sourceType;
	m_allowCursor = allowCursor;

	if (!QDBusConnection::sessionBus().isConnected()) {
		return false;
	}

	QVariantMap options;
	options.insert(QStringLiteral("session_handle_token"), m_token);

	return callWithRequest(QStringLiteral("CreateSession"), {}, options,
						   SLOT(onCreateSessionResponse(uint, QVariantMap)));
}

void PortalScreenCast::onCreateSessionResponse(std::uint32_t response, const QVariantMap &results) {
	if (response != PORTAL_RESPONSE_SUCCESS) {
		fail(tr("The desktop portal refused to start a screen sharing session."));

		return;
	}

	// The portal documentation types session_handle as an object path, and that is what current
	// xdg-desktop-portal sends; older releases sent a plain string. QVariant::toString() on a
	// QDBusObjectPath is empty, which is why this used to fail on every up-to-date desktop with "did
	// not return a session" in the very same second the share was started.
	const QVariant handle = results.value(QStringLiteral("session_handle"));
	QString path;

	if (handle.canConvert< QDBusObjectPath >()) {
		path = handle.value< QDBusObjectPath >().path();
	}

	if (path.isEmpty()) {
		path = handle.toString();
	}

	if (path.isEmpty()) {
		// Documented to be derivable from the token, so a portal that omits it is still usable.
		path = QStringLiteral("/org/freedesktop/portal/desktop/session/%1/%2").arg(senderPathElement(), m_token);
	}

	m_sessionHandle = QDBusObjectPath(path);
	m_sessionOpen   = true;

	std::uint32_t types = PORTAL_SOURCE_MONITOR | PORTAL_SOURCE_WINDOW;

	switch (m_sourceType) {
		case SourceType::Monitor:
			types = PORTAL_SOURCE_MONITOR;
			break;
		case SourceType::Window:
			types = PORTAL_SOURCE_WINDOW;
			break;
		case SourceType::Any:
			break;
	}

	QVariantMap options;
	options.insert(QStringLiteral("types"), types);
	// One source at a time: the encoder produces a single stream, and a second PipeWire node would have
	// nowhere to go.
	options.insert(QStringLiteral("multiple"), false);
	// 0 hides the pointer, 1 composites it into the frames. Embedded-in-stream is the only mode that
	// needs no separate metadata handling, which suits a source that just hands out QImages.
	options.insert(QStringLiteral("cursor_mode"), m_allowCursor ? 2u : 1u);

	if (!callWithRequest(QStringLiteral("SelectSources"), { QVariant::fromValue(m_sessionHandle) }, options,
						 SLOT(onSelectSourcesResponse(uint, QVariantMap)))) {
		fail(tr("Could not ask the desktop portal what to share."));
	}
}

void PortalScreenCast::onSelectSourcesResponse(std::uint32_t response, const QVariantMap &) {
	if (response != PORTAL_RESPONSE_SUCCESS) {
		// Cancelling is a decision, not a fault - reported plainly so the caller can distinguish it
		// from the portal being broken.
		fail(response == PORTAL_RESPONSE_CANCELLED ? tr("Screen sharing was cancelled.")
												   : tr("The desktop portal could not offer any sources to share."));

		return;
	}

	// Empty parent window: the portal dialog is not parented to ours. Passing a real handle needs the
	// xdg-foreign protocol on Wayland, which is more machinery than an unparented prompt is worth.
	if (!callWithRequest(QStringLiteral("Start"), { QVariant::fromValue(m_sessionHandle), QString() }, QVariantMap(),
						 SLOT(onStartResponse(uint, QVariantMap)))) {
		fail(tr("Could not start the screen sharing session."));
	}
}

void PortalScreenCast::onStartResponse(std::uint32_t response, const QVariantMap &results) {
	if (response != PORTAL_RESPONSE_SUCCESS) {
		fail(response == PORTAL_RESPONSE_CANCELLED ? tr("Screen sharing was cancelled.")
												   : tr("The desktop portal refused to share the screen."));

		return;
	}

	// streams is a(ua{sv}): a list of (node id, properties). multiple=false was requested, so there is
	// at most one, but a portal is free to return none if the user deselected everything.
	const QDBusArgument streams = results.value(QStringLiteral("streams")).value< QDBusArgument >();

	std::uint32_t nodeId = 0;
	QVariantMap streamProperties;

	streams.beginArray();

	if (streams.atEnd()) {
		streams.endArray();
		fail(tr("The desktop portal returned nothing to capture."));

		return;
	}

	streams.beginStructure();
	streams >> nodeId >> streamProperties;
	streams.endStructure();
	streams.endArray();

	if (nodeId == 0) {
		fail(tr("The desktop portal returned nothing to capture."));

		return;
	}

	m_nodeId = nodeId;

	// The portal knows what the user picked; asking it is better than guessing from the node id.
	const QString portalDescription = streamProperties.value(QStringLiteral("id")).toString();
	m_description = portalDescription.isEmpty() ? tr("Shared screen") : portalDescription;

	QDBusInterface iface(QLatin1String(PORTAL_SERVICE), QLatin1String(PORTAL_PATH),
						 QLatin1String(SCREENCAST_IFACE), QDBusConnection::sessionBus());

	const QDBusReply< QDBusUnixFileDescriptor > fdReply =
		iface.call(QStringLiteral("OpenPipeWireRemote"), QVariant::fromValue(m_sessionHandle), QVariantMap());

	if (!fdReply.isValid()) {
		fail(tr("Could not open the screen sharing stream."));

		return;
	}

	// Duplicated because the QDBusUnixFileDescriptor closes its copy when it goes out of scope, and the
	// PipeWire loop needs the descriptor to outlive this call.
	m_pipeWireFd = ::dup(fdReply.value().fileDescriptor());

	if (m_pipeWireFd < 0) {
		fail(tr("Could not open the screen sharing stream."));

		return;
	}

	emit ready();
}

void PortalScreenCast::close() {
	if (m_pipeWireFd >= 0) {
		::close(m_pipeWireFd);
		m_pipeWireFd = -1;
	}

	if (m_sessionOpen && !m_sessionHandle.path().isEmpty()) {
		QDBusInterface session(QLatin1String(PORTAL_SERVICE), m_sessionHandle.path(), QLatin1String(SESSION_IFACE),
							   QDBusConnection::sessionBus());

		// Asynchronous: this runs from the destructor as well, and a blocking call there would stall
		// shutdown if the portal is slow or already gone.
		session.asyncCall(QStringLiteral("Close"));

		m_sessionOpen = false;
	}

	m_nodeId = 0;
}
