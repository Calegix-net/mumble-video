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

QTEST_MAIN(TestVideoUi)
#include "TestVideoUi.moc"
