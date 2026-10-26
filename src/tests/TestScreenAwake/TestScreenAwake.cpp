// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "ScreenAwake.h"

#include <QtTest>

#ifdef USE_DBUS
#	include <QtDBus/QDBusConnection>
#	include <QtDBus/QDBusConnectionInterface>
#	include <QtDBus/QDBusObjectPath>
#	include <QtCore/QMutex>
#	include <QtCore/QThread>

#	include <atomic>

/// The portal's Request object for one inhibition: closing it lifts the inhibition.
class FakeRequest : public QObject {
	Q_OBJECT
	Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Request")

public:
	explicit FakeRequest(std::atomic< int > *open, QObject *parent) : QObject(parent), m_open(open) {}

public slots:
	void Close() {
		--*m_open;
		deleteLater();
	}

private:
	std::atomic< int > *m_open;
};

/// Stands in for xdg-desktop-portal's Inhibit interface.
class FakePortal : public QObject {
	Q_OBJECT
	Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Inhibit")

public:
	explicit FakePortal(QDBusConnection bus) : m_bus(bus) {}

	std::atomic< int > open{ 0 };
	std::atomic< int > calls{ 0 };
	std::atomic< unsigned int > flags{ 0 };

public slots:
	QDBusObjectPath Inhibit(const QString &, uint f, const QVariantMap &) {
		const int n = ++calls;
		++open;
		flags              = f;
		const QString path = QStringLiteral("/org/freedesktop/portal/desktop/request/test/r%1").arg(n);
		m_bus.registerObject(path, new FakeRequest(&open, this), QDBusConnection::ExportAllSlots);
		return QDBusObjectPath(path);
	}

private:
	QDBusConnection m_bus;
};

/// Stands in for org.freedesktop.ScreenSaver, for desktops without the portal.
class FakeScreenSaver : public QObject {
	Q_OBJECT
	Q_CLASSINFO("D-Bus Interface", "org.freedesktop.ScreenSaver")

public:
	std::atomic< int > held{ 0 };

public slots:
	uint Inhibit(const QString &, const QString &) {
		QMutexLocker lock(&m_mutex);
		m_cookies.insert(++m_next);
		held = static_cast< int >(m_cookies.size());
		return m_next;
	}
	void UnInhibit(uint cookie) {
		QMutexLocker lock(&m_mutex);
		m_cookies.remove(cookie);
		held = static_cast< int >(m_cookies.size());
	}

private:
	QMutex m_mutex;
	QSet< uint > m_cookies;
	uint m_next = 41;
};
#endif

class TestScreenAwake : public QObject {
	Q_OBJECT

private slots:
	void wantedFollowsTheSetting();
#ifdef USE_DBUS
	void initTestCase();
	void cleanupTestCase();
	void portalInhibitsAndCloses();
	void portalReleasedOnDestruction();
	void changeOfMindWhileAsking();
	void fallsBackToScreenSaver();
	void noServiceIsHarmless();

private:
	/// The fakes answer on a thread of their own, the way a real portal is another process: the client
	/// blocks on its release when it is destroyed, which would otherwise wait on itself.
	QThread m_serviceThread;
	template< typename T > T *onServiceThread(T *object) {
		object->moveToThread(&m_serviceThread);
		return object;
	}

	/// A second connection to the same bus owns the fake services, so calls really go over the bus.
	QDBusConnection serviceBus() { return QDBusConnection::connectToBus(QDBusConnection::SessionBus, "fake"); }
	void requireBus();
#endif
};

void TestScreenAwake::wantedFollowsTheSetting() {
	for (bool sending : { false, true }) {
		for (bool watching : { false, true }) {
			QCOMPARE(ScreenAwake::wanted(ScreenAwake::Off, sending, watching), false);
			QCOMPARE(ScreenAwake::wanted(ScreenAwake::WhileVideo, sending, watching), sending || watching);
			QCOMPARE(ScreenAwake::wanted(ScreenAwake::WhileRunning, sending, watching), true);
		}
	}
	// An unknown value from a newer or damaged settings file keeps the screen alone.
	QCOMPARE(ScreenAwake::wanted(7, true, true), false);
}

#ifdef USE_DBUS
void TestScreenAwake::initTestCase() {
	if (!QDBusConnection::sessionBus().isConnected()) {
		qInfo("No session bus: only the condition logic is tested (run under dbus-run-session for the rest)");
	}
	m_serviceThread.start();
}

void TestScreenAwake::cleanupTestCase() {
	m_serviceThread.quit();
	m_serviceThread.wait();
}

void TestScreenAwake::requireBus() {
	if (!QDBusConnection::sessionBus().isConnected()) {
		QSKIP("needs a session bus");
	}
	const QString owner = QDBusConnection::sessionBus().interface()->serviceOwner("org.freedesktop.portal.Desktop");
	if (!owner.isEmpty() && owner != serviceBus().baseService()) {
		QSKIP("a real portal owns the name on this bus; run under dbus-run-session");
	}
}

void TestScreenAwake::portalInhibitsAndCloses() {
	requireBus();
	QDBusConnection bus = serviceBus();
	FakePortal &portal = *onServiceThread(new FakePortal(bus));
	QVERIFY(bus.registerService("org.freedesktop.portal.Desktop"));
	QVERIFY(bus.registerObject("/org/freedesktop/portal/desktop", &portal, QDBusConnection::ExportAllSlots));

	{
		ScreenAwake awake;
		awake.setInhibited(true);
		QTRY_VERIFY(awake.isHeld());
		QCOMPARE(portal.open.load(), 1);
		QCOMPARE(portal.flags.load(), 8u);

		// Asking again does not stack a second inhibition.
		awake.setInhibited(true);
		QTest::qWait(50);
		QCOMPARE(portal.calls.load(), 1);

		awake.setInhibited(false);
		QVERIFY(!awake.isHeld());
		QTRY_COMPARE(portal.open.load(), 0);

		awake.setInhibited(true);
		QTRY_COMPARE(portal.open.load(), 1);
		awake.setInhibited(false);
		QTRY_COMPARE(portal.open.load(), 0);
		QCOMPARE(portal.calls.load(), 2);
	}

	bus.unregisterObject("/org/freedesktop/portal/desktop");
	bus.unregisterService("org.freedesktop.portal.Desktop");
	portal.deleteLater();
}

void TestScreenAwake::portalReleasedOnDestruction() {
	requireBus();
	QDBusConnection bus = serviceBus();
	FakePortal &portal = *onServiceThread(new FakePortal(bus));
	QVERIFY(bus.registerService("org.freedesktop.portal.Desktop"));
	QVERIFY(bus.registerObject("/org/freedesktop/portal/desktop", &portal, QDBusConnection::ExportAllSlots));

	{
		ScreenAwake awake;
		awake.setInhibited(true);
		QTRY_VERIFY(awake.isHeld());
	}
	QTRY_COMPARE(portal.open.load(), 0);

	// Destroyed while the Inhibit call is still unanswered: what it is granted is closed as well.
	{
		ScreenAwake awake;
		awake.setInhibited(true);
	}
	QTRY_COMPARE(portal.calls.load(), 2);
	QTRY_COMPARE(portal.open.load(), 0);

	bus.unregisterObject("/org/freedesktop/portal/desktop");
	bus.unregisterService("org.freedesktop.portal.Desktop");
	portal.deleteLater();
}

void TestScreenAwake::changeOfMindWhileAsking() {
	requireBus();
	QDBusConnection bus = serviceBus();
	FakePortal &portal = *onServiceThread(new FakePortal(bus));
	QVERIFY(bus.registerService("org.freedesktop.portal.Desktop"));
	QVERIFY(bus.registerObject("/org/freedesktop/portal/desktop", &portal, QDBusConnection::ExportAllSlots));

	ScreenAwake awake;
	awake.setInhibited(true);
	awake.setInhibited(false);
	QTRY_COMPARE(portal.calls.load(), 1);
	QTRY_COMPARE(portal.open.load(), 0);
	QVERIFY(!awake.isHeld());

	bus.unregisterObject("/org/freedesktop/portal/desktop");
	bus.unregisterService("org.freedesktop.portal.Desktop");
	portal.deleteLater();
}

void TestScreenAwake::fallsBackToScreenSaver() {
	requireBus();
	QDBusConnection bus = serviceBus();
	FakeScreenSaver &saver = *onServiceThread(new FakeScreenSaver);
	QVERIFY(bus.registerService("org.freedesktop.ScreenSaver"));
	QVERIFY(bus.registerObject("/org/freedesktop/ScreenSaver", &saver, QDBusConnection::ExportAllSlots));

	{
		ScreenAwake awake;
		awake.setInhibited(true);
		QTRY_VERIFY(awake.isHeld());
		QCOMPARE(saver.held.load(), 1);
		awake.setInhibited(false);
		QTRY_COMPARE(saver.held.load(), 0);

		awake.setInhibited(true);
		QTRY_COMPARE(saver.held.load(), 1);
	}
	QTRY_COMPARE(saver.held.load(), 0);

	bus.unregisterObject("/org/freedesktop/ScreenSaver");
	bus.unregisterService("org.freedesktop.ScreenSaver");
	saver.deleteLater();
}

void TestScreenAwake::noServiceIsHarmless() {
	requireBus();
	ScreenAwake awake;
	awake.setInhibited(true);
	QTest::qWait(200);
	QVERIFY(!awake.isHeld());
	awake.setInhibited(false);
}
#endif

QTEST_MAIN(TestScreenAwake)
#include "TestScreenAwake.moc"
