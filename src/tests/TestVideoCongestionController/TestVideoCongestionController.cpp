// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "VideoCongestionController.h"

#include <QtTest>

// Drives VideoCongestionController's control law directly, with a virtual clock, so the AIMD behaviour is
// checked without a network.
class TestVideoCongestionController : public QObject {
	Q_OBJECT

	// Feed one reporter a loss figure and advance one 1 s tick; returns the controller's new bitrate.
	static unsigned int step(VideoCongestionController &cc, std::int64_t &t, std::uint32_t reporter,
							 unsigned int lossPermille, std::uint64_t completeFrame) {
		cc.onFeedback(reporter, lossPermille, completeFrame, t);
		const unsigned int r = cc.tick(t);
		t += 1000;
		return r;
	}

private slots:
	void holdsWithoutFeedback() {
		VideoCongestionController cc(false);
		cc.setCeiling(2000);
		QCOMPARE(cc.currentBitrate(), 2000u);
		std::int64_t t = 0;
		for (int i = 0; i < 10; ++i) {
			cc.tick(t);
			t += 1000;
		}
		// No reporters ever spoke: must sit exactly at the configured rate.
		QCOMPARE(cc.currentBitrate(), 2000u);
	}

	void killSwitchPins() {
		qputenv("MUMBLE_VIDEO_CC", "0");
		VideoCongestionController cc(false);
		cc.setCeiling(2000);
		QVERIFY(!cc.enabled());
		std::int64_t t = 0;
		// Even under heavy sustained loss, a disabled controller never moves the rate.
		std::uint64_t f = 1;
		for (int i = 0; i < 12; ++i) {
			step(cc, t, 7, 300, f++);
		}
		QCOMPARE(cc.currentBitrate(), 2000u);
		qunsetenv("MUMBLE_VIDEO_CC");
	}

	void decreasesOnLoss() {
		VideoCongestionController cc(false);
		cc.setCeiling(2000);
		std::int64_t t  = 0;
		std::uint64_t f = 1;
		// Clear the 3 s join grace first (frames advancing so no stall override).
		for (int i = 0; i < 4; ++i) {
			step(cc, t, 7, 0, f++);
		}
		const unsigned int before = cc.currentBitrate();
		// A clear loss signal above the decrease threshold -> one multiplicative decrease.
		const unsigned int after = step(cc, t, 7, 60, f++);
		QVERIFY2(after < before, "a loss report above threshold must lower the rate");
		QVERIFY(after >= static_cast< unsigned int >(before * 0.80));   // ~0.85x, not a collapse
	}

	void severeLossDropsHarder() {
		VideoCongestionController mild(false), severe(false);
		mild.setCeiling(4000);
		severe.setCeiling(4000);
		std::int64_t t = 0;
		std::uint64_t f = 1;
		for (int i = 0; i < 4; ++i) {
			mild.onFeedback(1, 0, f, t);
			mild.tick(t);
			severe.onFeedback(1, 0, f, t);
			severe.tick(t);
			t += 1000;
			++f;
		}
		mild.onFeedback(1, 40, f, t);
		const unsigned int mildRate = mild.tick(t);
		severe.onFeedback(1, 200, f, t);
		const unsigned int severeRate = severe.tick(t);
		QVERIFY2(severeRate < mildRate, "severe loss should back off harder than moderate loss");
	}

	void neverBelowFloorNorAboveCeiling() {
		VideoCongestionController cc(false);
		cc.setCeiling(2000);
		std::int64_t t  = 0;
		std::uint64_t f = 1;
		// Pound it with maximum loss for a long time: it must bottom out at the floor, not zero.
		for (int i = 0; i < 60; ++i) {
			step(cc, t, 7, 1000, f);   // frame not advancing + max loss
		}
		QVERIFY(cc.currentBitrate() >= cc.floor());
		QVERIFY(cc.currentBitrate() <= 2000u);
		// Now clear the loss for a long time: it must climb back, never above the ceiling.
		for (int i = 0; i < 120; ++i) {
			step(cc, t, 7, 0, f++);
		}
		QVERIFY(cc.currentBitrate() <= 2000u);
		QVERIFY2(cc.currentBitrate() > cc.floor(), "should probe back up once the link clears");
	}

	void outlierReceiverDoesNotStarveTheRest() {
		VideoCongestionController cc(false);
		cc.setCeiling(3000);
		std::int64_t t  = 0;
		std::uint64_t f = 1;
		// Four viewers; one is on a bad link (high loss), three are fine. The one must be dropped as an
		// outlier so the rate stays high for the majority.
		for (int i = 0; i < 20; ++i) {
			cc.onFeedback(1, 0, f, t);
			cc.onFeedback(2, 0, f, t);
			cc.onFeedback(3, 0, f, t);
			cc.onFeedback(4, 300, f, t);   // the bad one
			cc.tick(t);
			t += 1000;
			++f;
		}
		QVERIFY2(cc.currentBitrate() >= 2500u, "one bad link among four must not drag the rate down");
	}

	void congestedMajorityBacksOff() {
		VideoCongestionController cc(false);
		cc.setCeiling(3000);
		std::int64_t t  = 0;
		std::uint64_t f = 1;
		// Two of four are congested: not an outlier, so the rate must come down.
		for (int i = 0; i < 20; ++i) {
			cc.onFeedback(1, 0, f, t);
			cc.onFeedback(2, 0, f, t);
			cc.onFeedback(3, 200, f, t);
			cc.onFeedback(4, 200, f, t);
			cc.tick(t);
			t += 1000;
			++f;
		}
		QVERIFY2(cc.currentBitrate() < 2500u, "two congested of four should back the rate off");
	}

	void nackPressureForcesDecrease() {
		VideoCongestionController cc(false);
		cc.setCeiling(2000);
		std::int64_t t  = 0;
		std::uint64_t f = 1;
		for (int i = 0; i < 4; ++i) {
			step(cc, t, 7, 0, f++);   // clear grace, reporter says no loss
		}
		const unsigned int before = cc.currentBitrate();
		// Three distinct viewers NACK this interval while the loss report claims zero: NACK pressure must
		// still force a decrease (NACKs saturate, so a low permille can hide heavy loss).
		cc.onFeedback(7, 0, f, t);
		cc.onNack(10, t);
		cc.onNack(11, t);
		cc.onNack(12, t);
		const unsigned int after = cc.tick(t);
		QVERIFY2(after < before, "NACK pressure from several viewers must lower the rate");
	}
};

QTEST_MAIN(TestVideoCongestionController)
#include "TestVideoCongestionController.moc"
