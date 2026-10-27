// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

// GlobalShortcutPortal against a stand-in for xdg-desktop-portal on a private bus.
//
// The stand-in answers the way the real portal does: every call returns a Request object path at once
// and the result arrives later as a Response signal on that path, Activated/Deactivated are broadcast
// on the portal object with the session handle in them. A client that subscribed late, ignored the
// session handle or never bound would fail one of these, which is the point - none of it can be seen
// in a desktop session without a person pressing keys.

#include "GlobalShortcutPortal.h"

#include <QtCore/QProcess>
#include <QtCore/QTemporaryFile>
#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusMessage>
#include <QtDBus/QDBusVirtualObject>
#include <QtTest>

namespace {

const QString PORTAL_SERVICE = QStringLiteral("org.freedesktop.portal.Desktop");
const QString PORTAL_PATH    = QStringLiteral("/org/freedesktop/portal/desktop");
const QString SHORTCUTS      = QStringLiteral("org.freedesktop.portal.GlobalShortcuts");

QString senderElement(const QString &sender) {
	QString element = sender;
	element.remove(0, 1);
	return element.replace(QLatin1Char('.'), QLatin1Char('_'));
}

} // namespace

class FakePortal : public QDBusVirtualObject {
	Q_OBJECT

public:
	explicit FakePortal(QDBusConnection bus) : m_bus(bus) {}

	QString introspect(const QString &) const override { return QString(); }

	bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override {
		if (message.interface() == QLatin1String("org.freedesktop.portal.Session")
			&& message.member() == QLatin1String("Close")) {
			++closed;
			connection.send(message.createReply());
			return true;
		}

		if (message.interface() != SHORTCUTS) {
			return false;
		}

		const QList< QVariant > args = message.arguments();
		const QVariantMap options    = qdbus_cast< QVariantMap >(args.last());
		const QString request = QStringLiteral("%1/request/%2/%3")
									.arg(PORTAL_PATH, senderElement(message.service()),
										 options.value(QStringLiteral("handle_token")).toString());

		if (message.member() == QLatin1String("CreateSession")) {
			session = QStringLiteral("%1/session/%2/%3")
						  .arg(PORTAL_PATH, senderElement(message.service()),
							   options.value(QStringLiteral("session_handle_token")).toString());
			connection.send(message.createReply(QVariant::fromValue(QDBusObjectPath(request))));

			QVariantMap results;
			results.insert(QStringLiteral("session_handle"), QVariant::fromValue(QDBusObjectPath(session)));
			respond(request, refuseSession ? 2 : 0, results);
			return true;
		}

		if (message.member() == QLatin1String("BindShortcuts")) {
			++binds;
			boundSession = qdbus_cast< QDBusObjectPath >(args.at(0)).path();
			offered      = qdbus_cast< PortalShortcutList >(args.at(1));
			connection.send(message.createReply(QVariant::fromValue(QDBusObjectPath(request))));

			// The desktop assigns a key to the first shortcut, as a person would in its dialog.
			PortalShortcutList assigned;
			if (!offered.isEmpty()) {
				QVariantMap properties = offered.first().properties;
				properties.insert(QStringLiteral("trigger_description"), QStringLiteral("F13"));
				assigned << PortalShortcut{ offered.first().id, properties };
			}
			QVariantMap results;
			results.insert(QStringLiteral("shortcuts"), QVariant::fromValue(assigned));
			respond(request, 0, results);
			return true;
		}

		if (message.member() == QLatin1String("ConfigureShortcuts")) {
			// A version 1 portal, which does not have this.
			connection.send(message.createErrorReply(QStringLiteral("org.freedesktop.DBus.Error.UnknownMethod"),
													 QStringLiteral("No such method")));
			return true;
		}

		return false;
	}

	void press(const QString &forSession, const QString &id, bool down) {
		QDBusMessage signal = QDBusMessage::createSignal(PORTAL_PATH, SHORTCUTS,
														 down ? QStringLiteral("Activated") : QStringLiteral("Deactivated"));
		signal << QVariant::fromValue(QDBusObjectPath(forSession)) << id << qulonglong(0) << QVariantMap();
		m_bus.send(signal);
	}

	QString session;
	QString boundSession;
	PortalShortcutList offered;
	int binds          = 0;
	int closed         = 0;
	bool refuseSession = false;

private:
	QDBusConnection m_bus;

	void respond(const QString &request, uint code, const QVariantMap &results) {
		QDBusMessage signal =
			QDBusMessage::createSignal(request, QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"));
		signal << code << results;
		m_bus.send(signal);
	}
};

class TestGlobalShortcutPortal : public QObject {
	Q_OBJECT

private:
	QProcess m_daemon;
	/// Holds the bus configuration for the daemon's lifetime; removed when this object is destroyed.
	QTemporaryFile m_config;
	/// Every connection connect() opened, so cleanup() can close them whether or not the test passed.
	QStringList m_openConnections;
	QString m_address;
	int m_connections = 0;

	QDBusConnection connect(const QString &name) {
		const QString unique = QStringLiteral("%1%2").arg(name).arg(++m_connections);
		m_openConnections << unique;

		return QDBusConnection::connectToBus(m_address, unique);
	}

	static PortalShortcutList shortcuts(const QStringList &ids) {
		PortalShortcutList list;
		for (const QString &id : ids) {
			QVariantMap properties;
			properties.insert(QStringLiteral("description"), QString(id + QStringLiteral(" action")));
			list << PortalShortcut{ id, properties };
		}
		return list;
	}

private slots:
	void initTestCase() {
		// A bus of our own, and - the part that matters - one that can activate nothing.
		//
		// `dbus-daemon --session` reads the machine's /usr/share/dbus-1/services/, which on any desktop
		// Linux box contains org.freedesktop.portal.Desktop.service. So the first test to call a method on
		// that name made this private bus start the machine's real /usr/libexec/xdg-desktop-portal, which
		// then owned the name for the rest of the process - and the later case that registers the name as
		// a fake portal could not, because a real portal had it. That failed on every developer machine
		// with a portal installed and passed in CI containers, which is the worst way round.
		//
		// It also meant noPortalIsReportedNotHung() passed for the wrong reason: it is meant to test
		// "nothing owns the portal name", and what it actually met was a real portal with no
		// GlobalShortcuts backend, which happens to report unavailable as well.
		//
		// An empty set of service directories is what makes the bus honest.
		QVERIFY(m_config.open());
		m_config.write("<busconfig>\n"
					   "  <type>session</type>\n"
					   "  <listen>unix:tmpdir=/tmp</listen>\n"
					   "  <policy context=\"default\">\n"
					   "    <allow send_destination=\"*\"/>\n"
					   "    <allow own=\"*\"/>\n"
					   "  </policy>\n"
					   "</busconfig>\n");
		m_config.flush();

		m_daemon.start(QStringLiteral("dbus-daemon"), { QStringLiteral("--config-file=") + m_config.fileName(),
														QStringLiteral("--nofork"),
														QStringLiteral("--print-address=1") });
		if (!m_daemon.waitForStarted(5000)) {
			QSKIP("dbus-daemon is not available");
		}
		QVERIFY(m_daemon.waitForReadyRead(5000));
		m_address = QString::fromUtf8(m_daemon.readLine()).trimmed();
		QVERIFY(!m_address.isEmpty());
	}

	void cleanupTestCase() {
		m_daemon.terminate();
		m_daemon.waitForFinished(5000);
	}

	/// QtTest runs this even after a failing QVERIFY, which the unregister calls at the end of each test
	/// body do not get. Without it one failure cascaded into every later case, because the fake portal's
	/// bus name stayed registered on a connection nobody closed.
	void cleanup() {
		for (const QString &name : m_openConnections) {
			QDBusConnection connection(name);

			if (connection.isConnected()) {
				connection.unregisterService(PORTAL_SERVICE);
				connection.unregisterObject(PORTAL_PATH, QDBusConnection::UnregisterTree);
			}

			QDBusConnection::disconnectFromBus(name);
		}

		m_openConnections.clear();
	}

	void bindsAndForwardsOnlyItsOwnSession() {
		QDBusConnection portalBus = connect(QStringLiteral("portal"));
		QVERIFY(portalBus.isConnected());
		FakePortal portal(portalBus);
		QVERIFY(portalBus.registerVirtualObject(PORTAL_PATH, &portal, QDBusConnection::SubPath));
		QVERIFY(portalBus.registerService(PORTAL_SERVICE));

		QDBusConnection clientBus = connect(QStringLiteral("client"));
		GlobalShortcutPortal client(clientBus);
		QSignalSpy bound(&client, &GlobalShortcutPortal::bound);
		QSignalSpy activated(&client, &GlobalShortcutPortal::activated);
		QSignalSpy deactivated(&client, &GlobalShortcutPortal::deactivated);
		QSignalSpy unavailable(&client, &GlobalShortcutPortal::unavailable);

		client.start(shortcuts({ QStringLiteral("gsPushTalk"), QStringLiteral("gsMuteSelf") }));
		QVERIFY(bound.wait(5000));
		QVERIFY(unavailable.isEmpty());
		QVERIFY(client.isActive());

		// Bound against the session the portal created, with every function offered and described.
		QCOMPARE(portal.boundSession, portal.session);
		QCOMPARE(portal.offered.size(), 2);
		QCOMPARE(portal.offered.at(0).id, QStringLiteral("gsPushTalk"));
		QCOMPARE(portal.offered.at(0).properties.value(QStringLiteral("description")).toString(),
				 QStringLiteral("gsPushTalk action"));
		QCOMPARE(client.triggerDescription(QStringLiteral("gsPushTalk")), QStringLiteral("F13"));
		QVERIFY(client.triggerDescription(QStringLiteral("gsMuteSelf")).isEmpty());

		// Another session's activation is not ours to act on.
		portal.press(QStringLiteral("/org/freedesktop/portal/desktop/session/other/x"), QStringLiteral("gsPushTalk"), true);
		portal.press(portal.session, QStringLiteral("gsPushTalk"), true);
		QVERIFY(activated.wait(5000));
		QCOMPARE(activated.size(), 1);
		QCOMPARE(activated.at(0).at(0).toString(), QStringLiteral("gsPushTalk"));

		portal.press(portal.session, QStringLiteral("gsPushTalk"), false);
		QVERIFY(deactivated.wait(5000));
		QCOMPARE(deactivated.at(0).at(0).toString(), QStringLiteral("gsPushTalk"));

		// A changed set is offered again, to the same session.
		client.setShortcuts(shortcuts({ QStringLiteral("gsPushTalk") }));
		QVERIFY(bound.wait(5000));
		QCOMPARE(portal.binds, 2);
		QCOMPARE(portal.offered.size(), 1);

		// ConfigureShortcuts missing (version 1) falls back to binding again, which brings the dialog back.
		client.configure();
		QVERIFY(bound.wait(5000));
		QCOMPARE(portal.binds, 3);

		// Stopped: the session is closed and activations no longer arrive.
		client.stop();
		QVERIFY(!client.isActive());
		QTRY_COMPARE_WITH_TIMEOUT(portal.closed, 1, 5000);
		portal.press(portal.session, QStringLiteral("gsPushTalk"), true);
		QTest::qWait(200);
		QCOMPARE(activated.size(), 1);

		portalBus.unregisterService(PORTAL_SERVICE);
		portalBus.unregisterObject(PORTAL_PATH, QDBusConnection::UnregisterTree);
	}

	void noPortalIsReportedNotHung() {
		// Nothing owns the portal name on this bus: the call fails and says so.
		QDBusConnection clientBus = connect(QStringLiteral("lonely"));
		GlobalShortcutPortal client(clientBus);
		QSignalSpy unavailable(&client, &GlobalShortcutPortal::unavailable);

		client.start(shortcuts({ QStringLiteral("gsPushTalk") }));
		QVERIFY(unavailable.wait(5000));
		QVERIFY(!client.isActive());
	}

	void refusedSessionIsReported() {
		QDBusConnection portalBus = connect(QStringLiteral("portal"));
		FakePortal portal(portalBus);
		portal.refuseSession = true;
		QVERIFY(portalBus.registerVirtualObject(PORTAL_PATH, &portal, QDBusConnection::SubPath));
		QVERIFY(portalBus.registerService(PORTAL_SERVICE));

		QDBusConnection clientBus = connect(QStringLiteral("client"));
		GlobalShortcutPortal client(clientBus);
		QSignalSpy unavailable(&client, &GlobalShortcutPortal::unavailable);

		client.start(shortcuts({ QStringLiteral("gsPushTalk") }));
		QVERIFY(unavailable.wait(5000));
		QCOMPARE(portal.binds, 0);

		portalBus.unregisterService(PORTAL_SERVICE);
		portalBus.unregisterObject(PORTAL_PATH, QDBusConnection::UnregisterTree);
	}
};

QTEST_GUILESS_MAIN(TestGlobalShortcutPortal)
#include "TestGlobalShortcutPortal.moc"
