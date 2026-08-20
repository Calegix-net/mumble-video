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
	m_token = QStringLiteral("mumble%1").arg(QRandomGenerator::global()->generate(), 0, 16);
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

bool PortalScreenCast::connectRequest(const QDBusObjectPath &path, const char *slot) {
	return QDBusConnection::sessionBus().connect(QLatin1String(PORTAL_SERVICE), path.path(),
												 QLatin1String(REQUEST_IFACE), QStringLiteral("Response"), this,
												 slot);
}

bool PortalScreenCast::requestAccess(SourceType sourceType, bool allowCursor) {
	m_sourceType  = sourceType;
	m_allowCursor = allowCursor;

	if (!QDBusConnection::sessionBus().isConnected()) {
		return false;
	}

	QDBusInterface iface(QLatin1String(PORTAL_SERVICE), QLatin1String(PORTAL_PATH),
						 QLatin1String(SCREENCAST_IFACE), QDBusConnection::sessionBus());

	if (!iface.isValid()) {
		return false;
	}

	QVariantMap options;
	options.insert(QStringLiteral("handle_token"), m_token);
	options.insert(QStringLiteral("session_handle_token"), m_token);

	const QDBusReply< QDBusObjectPath > reply = iface.call(QStringLiteral("CreateSession"), options);

	if (!reply.isValid()) {
		return false;
	}

	return connectRequest(reply.value(), SLOT(onCreateSessionResponse(uint, QVariantMap)));
}

void PortalScreenCast::onCreateSessionResponse(std::uint32_t response, const QVariantMap &results) {
	if (response != PORTAL_RESPONSE_SUCCESS) {
		fail(tr("The desktop portal refused to start a screen sharing session."));

		return;
	}

	m_sessionHandle = QDBusObjectPath(results.value(QStringLiteral("session_handle")).toString());
	m_sessionOpen   = true;

	if (m_sessionHandle.path().isEmpty()) {
		fail(tr("The desktop portal did not return a screen sharing session."));

		return;
	}

	QDBusInterface iface(QLatin1String(PORTAL_SERVICE), QLatin1String(PORTAL_PATH),
						 QLatin1String(SCREENCAST_IFACE), QDBusConnection::sessionBus());

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
	options.insert(QStringLiteral("handle_token"), m_token);
	options.insert(QStringLiteral("types"), types);
	// One source at a time: the encoder produces a single stream, and a second PipeWire node would have
	// nowhere to go.
	options.insert(QStringLiteral("multiple"), false);
	// 0 hides the pointer, 1 composites it into the frames. Embedded-in-stream is the only mode that
	// needs no separate metadata handling, which suits a source that just hands out QImages.
	options.insert(QStringLiteral("cursor_mode"), m_allowCursor ? 2u : 1u);

	const QDBusReply< QDBusObjectPath > reply =
		iface.call(QStringLiteral("SelectSources"), QVariant::fromValue(m_sessionHandle), options);

	if (!reply.isValid() || !connectRequest(reply.value(), SLOT(onSelectSourcesResponse(uint, QVariantMap)))) {
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

	QDBusInterface iface(QLatin1String(PORTAL_SERVICE), QLatin1String(PORTAL_PATH),
						 QLatin1String(SCREENCAST_IFACE), QDBusConnection::sessionBus());

	QVariantMap options;
	options.insert(QStringLiteral("handle_token"), m_token);

	// Empty parent window: the portal dialog is not parented to ours. Passing a real handle needs the
	// xdg-foreign protocol on Wayland, which is more machinery than an unparented prompt is worth.
	const QDBusReply< QDBusObjectPath > reply =
		iface.call(QStringLiteral("Start"), QVariant::fromValue(m_sessionHandle), QString(), options);

	if (!reply.isValid() || !connectRequest(reply.value(), SLOT(onStartResponse(uint, QVariantMap)))) {
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
