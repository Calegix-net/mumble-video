// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_GLOBALSHORTCUTPORTAL_H_
#define MUMBLE_MUMBLE_GLOBALSHORTCUTPORTAL_H_

#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QVariantMap>
#include <QtDBus/QDBusArgument>
#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusObjectPath>

#include <cstdint>

/// One entry of the portal's a(sa{sv}): a shortcut id and its properties ("description" going in,
/// "trigger_description" coming back).
struct PortalShortcut {
	QString id;
	QVariantMap properties;

	bool operator==(const PortalShortcut &other) const {
		return id == other.id && properties == other.properties;
	}
};
Q_DECLARE_METATYPE(PortalShortcut)
using PortalShortcutList = QList< PortalShortcut >;
Q_DECLARE_METATYPE(PortalShortcutList)

QDBusArgument &operator<<(QDBusArgument &argument, const PortalShortcut &shortcut);
const QDBusArgument &operator>>(const QDBusArgument &argument, PortalShortcut &shortcut);

/**
 * Global shortcuts through org.freedesktop.portal.GlobalShortcuts.
 *
 * On Wayland an application cannot watch the keyboard while it is not focused, which is the design, so
 * neither of the X11 backends can see a push-to-talk key pressed over another window, and inside the
 * flatpak there is no X display to ask at all. The portal is the sanctioned way round it: the
 * application names the actions it has, the desktop lets the user assign keys to them (GNOME 48+ and
 * KDE Plasma both implement it), and the portal reports when an assigned key goes down and up.
 *
 * So the model differs from the X11 one. Mumble does not see keys here, only actions: each global
 * shortcut function (Push-to-Talk, Mute Self, ...) is offered as one portal shortcut, and its key is
 * chosen in the desktop's dialog rather than in Mumble's shortcut table.
 *
 * Every call is asynchronous. The portal shows a dialog on BindShortcuts, and a blocking call would
 * freeze the client for as long as the user takes to answer it.
 */
class GlobalShortcutPortal : public QObject {
	Q_OBJECT

public:
	explicit GlobalShortcutPortal(QObject *parent = nullptr);
	/// For tests: talk to a portal on the given bus instead of the session bus.
	GlobalShortcutPortal(const QDBusConnection &bus, QObject *parent = nullptr);
	~GlobalShortcutPortal() override;

	/// Whether this is a session in which the portal is the way to get global shortcuts: a Wayland
	/// session with a session bus. Whether a portal actually implements GlobalShortcuts is only learned
	/// from start(), whose failure is reported through unavailable().
	static bool appliesToThisSession();

	/// Opens a portal session and offers the given shortcuts. Does nothing if already started.
	void start(const PortalShortcutList &shortcuts);

	/// Offers a changed set of shortcuts to the running session (or remembers it for start()).
	void setShortcuts(const PortalShortcutList &shortcuts);

	/// Asks the desktop to show its shortcut settings for this application (GlobalShortcuts version 2),
	/// falling back to binding again, which on older portals is what brings the dialog back.
	void configure();

	/// Closes the portal session. Activations stop arriving.
	void stop();

	bool isActive() const { return m_state == State::Bound; }

	/// The key the desktop reports for a shortcut id, or an empty string if none is assigned.
	QString triggerDescription(const QString &id) const;

signals:
	/// An assigned key went down.
	void activated(const QString &id);
	/// An assigned key went up.
	void deactivated(const QString &id);
	/// The portal accepted the shortcuts; activations can now arrive.
	void bound();
	/// The keys assigned to the shortcuts changed (after a bind or in the desktop's settings).
	void shortcutsChanged();
	/// There is no portal here that implements GlobalShortcuts, or it refused the session.
	void unavailable(const QString &reason);

private slots:
	void onCreateSessionResponse(uint response, const QVariantMap &results);
	void onBindShortcutsResponse(uint response, const QVariantMap &results);
	void onActivated(const QDBusObjectPath &session, const QString &id, qulonglong timestamp,
					 const QVariantMap &options);
	void onDeactivated(const QDBusObjectPath &session, const QString &id, qulonglong timestamp,
					   const QVariantMap &options);
	void onShortcutsChanged(const QDBusObjectPath &session, const PortalShortcutList &shortcuts);

private:
	enum class State { Idle, Creating, Binding, Bound, Failed };

	QDBusConnection m_bus;
	State m_state = State::Idle;
	QString m_sessionHandle;
	QString m_requestPath;
	PortalShortcutList m_offered;
	PortalShortcutList m_assigned;
	bool m_rebindPending = false;

	void init();
	QString senderPathElement() const;
	static QString newToken();
	bool callWithRequest(const QString &method, QList< QVariant > arguments, QVariantMap options,
						 const char *slot);
	void dropRequest();
	void bind();
	void updateAssigned(const QVariant &shortcuts);
	void fail(const QString &reason);
};

#endif // MUMBLE_MUMBLE_GLOBALSHORTCUTPORTAL_H_
