// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_PORTALSCREENCAST_H_
#define MUMBLE_MUMBLE_PORTALSCREENCAST_H_

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtDBus/QDBusObjectPath>

#include <cstdint>

/**
 * Obtains permission to capture the screen, via the XDG desktop portal.
 *
 * On Wayland an application cannot read the screen: the compositor will not let it, which is the whole
 * point of the design. Capture goes through org.freedesktop.portal.ScreenCast instead - the portal
 * shows its own picker, the user chooses what to share, and the application receives a PipeWire node
 * it may read and nothing else. The user's choice of display or window is made in the portal's dialog,
 * not ours, so there is no equivalent here of the Windows picker dialog.
 *
 * This runs on X11 too, where portals are also available; falling back to XGetImage would work but is
 * deliberately not implemented, because it would silently bypass the consent step that is the reason
 * this class exists.
 *
 * The protocol is a sequence of asynchronous calls, each answered on a Request object rather than by
 * the method's return: CreateSession, SelectSources, Start, then OpenPipeWireRemote. Nothing is
 * captured until the user has accepted, so a portal session cannot be established without a visible
 * prompt.
 */
class PortalScreenCast : public QObject {
	Q_OBJECT

public:
	/// What the user is being asked to share. The portal shows the corresponding tabs in its picker.
	enum class SourceType {
		Monitor,
		Window,
		Any,
	};

	explicit PortalScreenCast(QObject *parent = nullptr);
	~PortalScreenCast() override;

	/// Whether a portal implementation is present on the session bus. False on a machine with no
	/// desktop portal, where screen sharing cannot work and should not be offered.
	static bool isAvailable();

	/**
	 * Begins the permission sequence. Returns false only if the request could not be sent at all;
	 * acceptance or refusal by the user arrives later through ready() or failed().
	 *
	 * @param sourceType What to offer in the portal's picker.
	 * @param allowCursor Whether the pointer should be composited into the captured frames.
	 */
	bool requestAccess(SourceType sourceType, bool allowCursor);

	/// Closes the portal session. The compositor stops capturing as soon as this returns.
	void close();

	/// PipeWire node id to read frames from, valid only after ready().
	std::uint32_t nodeId() const { return m_nodeId; }

	/// File descriptor for the PipeWire remote, valid only after ready(). Owned by this object.
	int pipeWireFd() const { return m_pipeWireFd; }

	/// What the portal reported the user chose, for the UI. Empty until ready().
	QString describe() const { return m_description; }

signals:
	/// The user accepted and a PipeWire node is available.
	void ready();

	/// The user refused, or the portal failed. The message is suitable for showing to the user.
	void failed(const QString &reason);

protected slots:
	void onCreateSessionResponse(std::uint32_t response, const QVariantMap &results);
	void onSelectSourcesResponse(std::uint32_t response, const QVariantMap &results);
	void onStartResponse(std::uint32_t response, const QVariantMap &results);

protected:
	/**
	 * Subscribes to the Response signal of a portal Request object.
	 *
	 * The portal returns a Request path from each call and answers on that object. The path is
	 * predictable from the caller's unique bus name and the handle token, but the documented approach
	 * is to subscribe to whatever path the call returned, which is what this does - subscribing to a
	 * guessed path races with the portal replying before the subscription lands.
	 */
	bool connectRequest(const QDBusObjectPath &path, const char *slot);

	/// Token unique to this session, so concurrent requests from the same client do not collide.
	QString m_token;

	QDBusObjectPath m_sessionHandle;
	bool m_sessionOpen = false;

	SourceType m_sourceType = SourceType::Any;
	bool m_allowCursor      = false;

	std::uint32_t m_nodeId = 0;
	int m_pipeWireFd       = -1;

	QString m_description;

	void fail(const QString &reason);
};

#endif // MUMBLE_MUMBLE_PORTALSCREENCAST_H_
