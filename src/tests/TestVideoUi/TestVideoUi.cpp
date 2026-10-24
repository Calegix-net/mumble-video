// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

// The bandwidth measurement behind the video setup wizard.
//
// This is the calculation a user is asked to trust and has no way to check: it tells them what a bitrate
// setting will actually cost, and they find out whether it was right when a call goes badly. It was
// originally written inside the wizard dialog, where testing it would have needed a camera, a window and
// the client's settings singleton. Pulling it out into VideoBandwidthProbe made it ordinary domain logic
// that runs against a synthetic camera with no UI at all - which is the only reason these tests exist.

#include "CroppedVideoSource.h"
#include "VideoBandwidthProbe.h"
#include "VideoSource.h"

#include <QObject>
#include <QSignalSpy>
#include <QtTest>

#include <memory>

namespace {

/// Runs a probe to completion, or fails the calling test.
bool runToCompletion(VideoBandwidthProbe &probe, int budgetMs) {
	QElapsedTimer timer;
	timer.start();

	while (probe.isRunning() && timer.elapsed() < budgetMs) {
		QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
		QThread::msleep(10);
	}

	return !probe.isRunning();
}

std::unique_ptr< SyntheticVideoSource > movingSource(int w = 320, int h = 240) {
	auto source = std::make_unique< SyntheticVideoSource >(w, h);
	// Everything changes every frame, as a camera does, so the encoder has real work and rate control
	// actually engages.
	source->setChangeRatio(100);
	// And frames have to arrive on their own: the probe is driven by capture callbacks, not by polling.
	source->setInterval(33);

	return source;
}

} // namespace


namespace {
/// A source whose frames the test chooses, which SyntheticVideoSource cannot do - it renders its own
/// pattern. Cropping has to be checked against known pixels at known positions, or a crop of the right
/// size taken from the wrong place would pass.
class ScriptedVideoSource : public VideoSource {
	Q_OBJECT
public:
	explicit ScriptedVideoSource(QObject *parent = nullptr) : VideoSource(parent) {}

	bool start() override {
		m_running = true;
		return true;
	}
	void stop() override { m_running = false; }
	bool isRunning() const override { return m_running; }
	QString describe() const override { return QStringLiteral("Scripted"); }

	void deliver(const QImage &frame, std::uint64_t captureTimestampUsec) {
		emit frameReady(frame, captureTimestampUsec);
	}

private:
	bool m_running = false;
};
} // namespace

class TestVideoUi : public QObject {
	Q_OBJECT
private slots:
	void aSweepProducesOneResultPerCandidate();
	void higherTargetsMeasureHigher();
	void resultsAreNeverZero();
	void progressAdvancesAndCompletes();
	void eachCandidateIsReportedAsItCompletes();
	void aRefusedSourceIsReportedRatherThanHanging();
	void emptyCandidatesAreRejected();
	void stoppingEarlyLeavesItIdle();

	void aSelectionIsConvertedToTheDisplaysOwnPixels();
	void aRegionIsClampedToTheFrameAndNeverWidened();
	void croppingEmitsOnlyTheSelectedRectangle();
	void aRegionThatLeavesTheScreenReportsOnceInsteadOfSharingEverything();
};

void TestVideoUi::aSweepProducesOneResultPerCandidate() {
	VideoBandwidthProbe probe;

	const std::vector< unsigned int > candidates = { 300, 1200 };

	QVERIFY(probe.start(movingSource(), QSize(320, 240), 30, candidates, 400));
	QVERIFY2(runToCompletion(probe, 20000), "the sweep never finished");

	QCOMPARE(probe.results().size(), candidates.size());
	QCOMPARE(probe.candidates(), candidates);
}

void TestVideoUi::higherTargetsMeasureHigher() {
	VideoBandwidthProbe probe;

	QVERIFY(probe.start(movingSource(), QSize(320, 240), 30, { 250, 2500 }, 700));
	QVERIFY(runToCompletion(probe, 20000));

	QCOMPARE(probe.results().size(), static_cast< std::size_t >(2));

	// Rate control is a target rather than a guarantee, so this asserts the ordering rather than the
	// numbers. If asking for ten times the bitrate did not produce more data, the recommendation this
	// feeds would be meaningless - which is exactly the failure that hid behind a broken timebase
	// earlier: every target produced byte-identical output and nothing noticed.
	QVERIFY2(probe.results()[1] > probe.results()[0],
			 qPrintable(QStringLiteral("250 kbit/s measured %1, 2500 kbit/s measured %2")
							.arg(probe.results()[0], 0, 'f', 1)
							.arg(probe.results()[1], 0, 'f', 1)));
}

void TestVideoUi::resultsAreNeverZero() {
	VideoBandwidthProbe probe;

	QVERIFY(probe.start(movingSource(), QSize(320, 240), 30, { 800 }, 600));
	QVERIFY(runToCompletion(probe, 20000));

	QVERIFY(!probe.results().empty());

	// A zero would mean the sweep ran but encoded nothing, and would surface to the user as a confident
	// recommendation of a number that measured nothing at all.
	QVERIFY2(probe.results().front() > 0.0, "a candidate measured zero bitrate");
}

void TestVideoUi::progressAdvancesAndCompletes() {
	VideoBandwidthProbe probe;

	QVERIFY(probe.start(movingSource(), QSize(320, 240), 30, { 500, 900 }, 400));

	const int atStart = probe.progress();
	QVERIFY(atStart >= 0 && atStart <= 100);

	QVERIFY(runToCompletion(probe, 20000));
	QCOMPARE(probe.progress(), 100);
	QCOMPARE(probe.currentCandidate(), static_cast< std::size_t >(2));
}

void TestVideoUi::eachCandidateIsReportedAsItCompletes() {
	VideoBandwidthProbe probe;

	QSignalSpy measured(&probe, &VideoBandwidthProbe::candidateMeasured);
	QSignalSpy finished(&probe, &VideoBandwidthProbe::finished);

	QVERIFY(probe.start(movingSource(), QSize(320, 240), 30, { 400, 800, 1600 }, 350));
	QVERIFY(runToCompletion(probe, 25000));

	// Reported one at a time rather than only at the end, so a caller can show honest progress instead of
	// a bar that sits still and then jumps.
	QCOMPARE(measured.count(), 3);
	QCOMPARE(finished.count(), 1);

	QCOMPARE(measured.at(0).at(0).toUInt(), 400u);
	QCOMPARE(measured.at(1).at(0).toUInt(), 800u);
	QCOMPARE(measured.at(2).at(0).toUInt(), 1600u);

	for (int i = 0; i < measured.count(); ++i) {
		QVERIFY(measured.at(i).at(1).toDouble() > 0.0);
	}
}

void TestVideoUi::aRefusedSourceIsReportedRatherThanHanging() {
	VideoBandwidthProbe probe;

	// A null source is what a missing or busy camera looks like from here. It must be refused up front,
	// rather than leaving the caller waiting on a finished() that never arrives.
	QVERIFY(!probe.start(nullptr, QSize(320, 240), 30, { 800 }));
	QVERIFY(!probe.isRunning());
	QVERIFY(probe.results().empty());
}

void TestVideoUi::emptyCandidatesAreRejected() {
	VideoBandwidthProbe probe;

	QVERIFY(!probe.start(movingSource(), QSize(320, 240), 30, {}));
	QVERIFY(!probe.isRunning());
}

void TestVideoUi::stoppingEarlyLeavesItIdle() {
	VideoBandwidthProbe probe;

	QVERIFY(probe.start(movingSource(), QSize(320, 240), 30, { 800, 1600 }, 5000));
	QVERIFY(probe.isRunning());

	probe.stop();

	QVERIFY(!probe.isRunning());

	// Stopping twice, and after completion, must both be harmless: the wizard stops it whenever a page
	// changes without tracking whether it was running.
	probe.stop();
	QVERIFY(!probe.isRunning());
}


// A selection is drawn in Qt's device-independent pixels; the capture backend hands back the display's
// real ones. On a scaled display those differ, and using the drawn numbers unconverted shares the wrong
// part of the screen - a box around the right-hand half of a 150% display would describe its middle.
void TestVideoUi::aSelectionIsConvertedToTheDisplaysOwnPixels() {
	// Unscaled: the numbers are already the display's own.
	QCOMPARE(CroppedVideoSource::toCapturePixels(QRect(10, 20, 300, 200), 1.0), QRect(10, 20, 300, 200));

	// 200%: every edge doubles.
	QCOMPARE(CroppedVideoSource::toCapturePixels(QRect(10, 20, 300, 200), 2.0), QRect(20, 40, 600, 400));

	// 150%, where the arithmetic stops being exact. Edges are converted, not the origin and the size
	// separately, so the right edge lands at round(410 * 1.5) = 615 and the width follows from it.
	const QRect scaled = CroppedVideoSource::toCapturePixels(QRect(10, 20, 400, 200), 1.5);
	QCOMPARE(scaled.left(), 15);
	QCOMPARE(scaled.top(), 30);
	QCOMPARE(scaled.right() + 1, 615);
	QCOMPARE(scaled.width(), 600);

	// Two boxes that share an edge still share it afterwards: no seam, no overlap.
	const QRect leftHalf  = CroppedVideoSource::toCapturePixels(QRect(0, 0, 101, 50), 1.5);
	const QRect rightHalf = CroppedVideoSource::toCapturePixels(QRect(101, 0, 99, 50), 1.5);
	QCOMPARE(leftHalf.right() + 1, rightHalf.left());

	// A drag is symmetric: dragging bottom-right to top-left selects what dragging the other way would.
	// Built through rectFromDragPoints() rather than QRect's own two-point constructor, which treats its
	// second point as an inclusive bottom-right and so loses a pixel per axis when handed them reversed.
	const QRect forward  = CroppedVideoSource::rectFromDragPoints(QPoint(10, 20), QPoint(310, 220));
	const QRect backward = CroppedVideoSource::rectFromDragPoints(QPoint(310, 220), QPoint(10, 20));
	QCOMPARE(forward, backward);
	QCOMPARE(forward, QRect(10, 20, 301, 201));
	QCOMPARE(CroppedVideoSource::toCapturePixels(forward, 1.0), CroppedVideoSource::toCapturePixels(backward, 1.0));

	QVERIFY(CroppedVideoSource::toCapturePixels(QRect(), 1.0).isNull());
	QVERIFY(CroppedVideoSource::toCapturePixels(QRect(0, 0, 10, 10), 0.0).isNull());
}

// The frame can turn out smaller than the region - the display's resolution dropped, or it was replaced
// by a smaller one. The region shrinks to what is left; it must never grow to cover more of the screen
// than was selected, which is the one way this feature could leak something.
void TestVideoUi::aRegionIsClampedToTheFrameAndNeverWidened() {
	// Wholly inside: unchanged.
	QCOMPARE(CroppedVideoSource::clampToFrame(QRect(10, 10, 100, 80), QSize(640, 480)), QRect(10, 10, 100, 80));

	// Hanging off the right and bottom: trimmed to the frame, not moved back inside it.
	QCOMPARE(CroppedVideoSource::clampToFrame(QRect(600, 440, 100, 80), QSize(640, 480)), QRect(600, 440, 40, 40));

	// Odd dimensions come back even, because 4:2:0 needs them to be - matching VpxCodec's rounding so
	// the frame handed on is already the size that will be encoded.
	QCOMPARE(CroppedVideoSource::clampToFrame(QRect(0, 0, 101, 83), QSize(640, 480)), QRect(0, 0, 100, 82));

	// Entirely outside, or too small to survive rounding: nothing.
	QVERIFY(CroppedVideoSource::clampToFrame(QRect(700, 500, 100, 80), QSize(640, 480)).isNull());
	QVERIFY(CroppedVideoSource::clampToFrame(QRect(639, 0, 1, 50), QSize(640, 480)).isNull());
	QVERIFY(CroppedVideoSource::clampToFrame(QRect(0, 0, 100, 80), QSize()).isNull());
}

// The whole point: what goes out is the selected rectangle's pixels, not the screen's.
void TestVideoUi::croppingEmitsOnlyTheSelectedRectangle() {
	QImage screen(200, 100, QImage::Format_RGB32);
	screen.fill(Qt::black);

	// A red patch exactly where the region will be, so a crop of the right size taken from the wrong
	// place fails on colour rather than passing on size.
	for (int y = 20; y < 60; ++y) {
		for (int x = 40; x < 120; ++x) {
			screen.setPixel(x, y, qRgb(255, 0, 0));
		}
	}

	auto inner     = std::make_unique< ScriptedVideoSource >();
	auto *innerPtr = inner.get();

	CroppedVideoSource cropped(std::move(inner), QRect(40, 20, 80, 40));

	QSignalSpy frames(&cropped, &VideoSource::frameReady);
	QVERIFY(cropped.start());
	QVERIFY(cropped.isRunning());

	innerPtr->deliver(screen, 1234);

	QCOMPARE(frames.count(), 1);

	const QList< QVariant > emitted = frames.takeFirst();
	const QImage out                = emitted.at(0).value< QImage >();

	QCOMPARE(out.size(), QSize(80, 40));
	QCOMPARE(out.pixel(0, 0), qRgb(255, 0, 0));
	QCOMPARE(out.pixel(79, 39), qRgb(255, 0, 0));

	// The capture timestamp is passed through untouched: it is the only timing anchor the protocol has.
	QCOMPARE(emitted.at(1).value< std::uint64_t >(), static_cast< std::uint64_t >(1234));
}

// If the region falls off the screen entirely, say so once. Sending the whole frame instead would share
// more than the user chose, and sending nothing silently looks like the share froze.
void TestVideoUi::aRegionThatLeavesTheScreenReportsOnceInsteadOfSharingEverything() {
	auto inner     = std::make_unique< ScriptedVideoSource >();
	auto *innerPtr = inner.get();

	CroppedVideoSource cropped(std::move(inner), QRect(400, 300, 80, 40));

	QSignalSpy frames(&cropped, &VideoSource::frameReady);
	QSignalSpy failures(&cropped, &VideoSource::failed);

	QVERIFY(cropped.start());

	QImage screen(200, 100, QImage::Format_RGB32);
	screen.fill(Qt::black);

	innerPtr->deliver(screen, 1);
	innerPtr->deliver(screen, 2);
	innerPtr->deliver(screen, 3);

	QCOMPARE(frames.count(), 0);
	QCOMPARE(failures.count(), 1);
}

QTEST_MAIN(TestVideoUi)
#include "TestVideoUi.moc"
