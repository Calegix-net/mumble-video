// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "X11ScreenVideoSource.h"

#include <QGuiApplication>
#include <QScreen>
#include <QSignalSpy>
#include <QtTest>

/**
 * The X11 screen-capture fallback.
 *
 * Everything that needs a real X server is skipped without one, so this is safe on a CI runner and
 * meaningful under Xvfb, which supports MIT-SHM and therefore exercises the same path a desktop does.
 * What is checked without any display at all still matters: it is the refusal, and a refusal that
 * crashes or lies is how the Share Screen button ends up broken instead of absent.
 */
class TestX11ScreenCapture : public QObject {
	Q_OBJECT

private:
	/// The display's full extent, or a null rect when there is nothing to capture from.
	static QRect displayGeometry() {
		const QScreen *screen = QGuiApplication::primaryScreen();

		if (!screen || QGuiApplication::platformName().compare(QLatin1String("xcb")) != 0) {
			return {};
		}

		return screen->geometry();
	}

private slots:
	void aRegionLargerThanTheLimitIsRefused();
	void anEmptyRegionIsRefused();
	void aStoppedSourceReportsItself();
	void capturesAFrameOfTheRequestedSize();
	void aRegionOutsideTheDisplayFailsInsteadOfExiting();
	void anUnchangedScreenReportsIdleRatherThanAFrame();
};

// MAX_DIMENSION exists because a frame is allocated per capture; a region beyond it is a caller bug and
// has to be refused at start() rather than turned into a 500 MB allocation.
void TestX11ScreenCapture::aRegionLargerThanTheLimitIsRefused() {
	X11ScreenVideoSource source(QRect(0, 0, X11ScreenVideoSource::MAX_DIMENSION + 1, 100), QStringLiteral("huge"), 66);

	QVERIFY(!source.start());
	QVERIFY(!source.isRunning());
}

void TestX11ScreenCapture::anEmptyRegionIsRefused() {
	X11ScreenVideoSource source(QRect(0, 0, 0, 0), QStringLiteral("empty"), 66);

	QVERIFY(!source.start());
	QVERIFY(!source.isRunning());
}

void TestX11ScreenCapture::aStoppedSourceReportsItself() {
	X11ScreenVideoSource source(QRect(0, 0, 64, 64), QStringLiteral("Screen HDMI-1 (X11)"), 66);

	QVERIFY(!source.isRunning());
	QCOMPARE(source.describe(), QStringLiteral("Screen HDMI-1 (X11)"));

	// stop() before start() must be a no-op rather than tearing down state that was never built.
	source.stop();
	QVERIFY(!source.isRunning());
}

void TestX11ScreenCapture::capturesAFrameOfTheRequestedSize() {
	const QRect display = displayGeometry();

	if (display.isEmpty()) {
		QSKIP("needs an X11 display (run under Xvfb)");
	}

	QVERIFY(X11ScreenVideoSource::isAvailable());

	const QRect region(0, 0, qMin(320, display.width()), qMin(240, display.height()));
	X11ScreenVideoSource source(region, QStringLiteral("test"), 10);

	QSignalSpy frames(&source, &VideoSource::frameReady);
	QSignalSpy failures(&source, &VideoSource::failed);

	QVERIFY(source.start());
	QVERIFY(source.isRunning());

	QVERIFY2(frames.wait(5000), "no frame arrived from the X server");
	QCOMPARE(failures.count(), 0);

	const QImage frame = frames.first().at(0).value< QImage >();
	QCOMPARE(frame.size(), region.size());
	QCOMPARE(frame.format(), QImage::Format_RGB32);
	QVERIFY(!frame.isNull());

	// The timestamp is the protocol's only timing anchor; a frame carrying zero would pin every frame to
	// the same instant downstream.
	QVERIFY(frames.first().at(1).value< std::uint64_t >() > 0);

	source.stop();
	QVERIFY(!source.isRunning());
}

// The guard that matters most: a region reaching past the root window is a BadMatch, and with no error
// handler installed Xlib turns that into exit() of the whole client. Unplugging a monitor mid-share is
// the way a user gets there.
void TestX11ScreenCapture::aRegionOutsideTheDisplayFailsInsteadOfExiting() {
	const QRect display = displayGeometry();

	if (display.isEmpty()) {
		QSKIP("needs an X11 display (run under Xvfb)");
	}

	X11ScreenVideoSource source(QRect(display.width() - 8, display.height() - 8, 256, 256), QStringLiteral("offscreen"),
								10);

	QSignalSpy failures(&source, &VideoSource::failed);
	QSignalSpy frames(&source, &VideoSource::frameReady);

	QVERIFY(source.start());
	QVERIFY2(failures.wait(5000), "an out-of-bounds region neither failed nor produced a frame");
	QCOMPARE(frames.count(), 0);
	QVERIFY(!source.isRunning());
	QVERIFY(!failures.first().at(0).toString().isEmpty());
}

// A screen that is not changing should say so rather than hand the encoder an identical frame to diff
// tile by tile. Xvfb with nothing running on it is exactly that screen.
void TestX11ScreenCapture::anUnchangedScreenReportsIdleRatherThanAFrame() {
	const QRect display = displayGeometry();

	if (display.isEmpty()) {
		QSKIP("needs an X11 display (run under Xvfb)");
	}

	X11ScreenVideoSource source(QRect(0, 0, qMin(160, display.width()), qMin(120, display.height())),
								QStringLiteral("test"), 10);

	QSignalSpy idle(&source, &VideoSource::captureIdle);
	QSignalSpy frames(&source, &VideoSource::frameReady);

	QVERIFY(source.start());
	QVERIFY(frames.wait(5000));

	// The first poll always produces a frame - there is nothing to compare it against yet. Later polls of
	// a static screen must come back idle.
	QVERIFY2(idle.wait(5000), "a static screen kept producing frames instead of reporting idle");
	QCOMPARE(frames.count(), 1);

	source.stop();
}

QTEST_MAIN(TestX11ScreenCapture)
#include "TestX11ScreenCapture.moc"
