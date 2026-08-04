// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

// The send-side controller: what the "Share Camera" button actually drives.
//
// Tested against a synthetic source, so this runs with no camera, no window and no network - which is
// exactly why the capture abstraction exists.

#include "VideoBroadcaster.h"
#include "VideoSource.h"

#include <QObject>
#include <QSignalSpy>
#include <QtTest>

#include <memory>

class TestVideoBroadcaster : public QObject {
	Q_OBJECT
private slots:
	void startingAndStoppingReportsItself();
	void framesBecomeUnits();
	void anUnchangedPictureSendsNothing();
	void restartingAllocatesANewStream();
	void frameNumbersNeverRepeat();
	void requestingAKeyframeResendsEverything();
	void startingWithNoSourceFails();
	void vp8SendsOneWholeFrameUnit();
	void theCodecCanBeSwitched();
};

void TestVideoBroadcaster::startingAndStoppingReportsItself() {
	VideoBroadcaster broadcaster;
	QSignalSpy spy(&broadcaster, &VideoBroadcaster::activeChanged);

	QVERIFY(!broadcaster.isActive());

	QVERIFY(broadcaster.start(std::make_unique< SyntheticVideoSource >(160, 120)));
	QVERIFY(broadcaster.isActive());
	QCOMPARE(spy.count(), 1);
	QCOMPARE(spy.takeFirst().at(0).toBool(), true);

	broadcaster.stop();
	QVERIFY(!broadcaster.isActive());
	QCOMPARE(spy.count(), 1);
	QCOMPARE(spy.takeFirst().at(0).toBool(), false);

	// Stopping twice is not an event.
	broadcaster.stop();
	QCOMPARE(spy.count(), 0);
}

void TestVideoBroadcaster::framesBecomeUnits() {
	VideoBroadcaster broadcaster;
	// Tiled image: one unit per tile. Stated explicitly rather than relying on the default codec, which
	// is VP8 and produces one whole-frame unit instead.
	broadcaster.setCodec(1);

	auto owned                   = std::make_unique< SyntheticVideoSource >(320, 240);
	SyntheticVideoSource *source = owned.get();

	QVERIFY(broadcaster.start(std::move(owned)));

	QSignalSpy spy(&broadcaster, &VideoBroadcaster::unitReady);

	source->pump(1234);

	// A 320x240 frame at the default 128px tile size is a 3x2 grid, and the first frame of a stream is
	// sent in full.
	QCOMPARE(spy.count(), 6);

	const Mumble::Protocol::VideoUnitHeader header = spy.at(0).at(0).value< Mumble::Protocol::VideoUnitHeader >();

	QCOMPARE(header.streamID, broadcaster.streamID());
	QCOMPARE(header.frameNumber, static_cast< std::uint64_t >(0));
	QCOMPARE(header.captureTimestampUsec, static_cast< std::uint64_t >(1234));
	QVERIFY(header.isKeyframe);

	// Every tile carries real bytes.
	for (int i = 0; i < spy.count(); ++i) {
		QVERIFY(!spy.at(i).at(1).toByteArray().isEmpty());
	}
}

void TestVideoBroadcaster::anUnchangedPictureSendsNothing() {
	VideoBroadcaster broadcaster;
	// Skipping unchanged content is a property of the tiled codec specifically. VP8 runs at a constant
	// bitrate and sends a frame regardless, so this assertion is meaningless for it.
	broadcaster.setCodec(1);

	auto owned                   = std::make_unique< SyntheticVideoSource >(320, 240);
	SyntheticVideoSource *source = owned.get();
	source->setChangeRatio(0);

	QVERIFY(broadcaster.start(std::move(owned)));

	QSignalSpy spy(&broadcaster, &VideoBroadcaster::unitReady);

	source->pump(1);
	const qsizetype firstFrame = spy.count();
	QVERIFY(firstFrame > 0);

	spy.clear();

	// The same picture again. Nothing a receiver needs, so nothing goes on the wire -- this is what makes
	// a static screen share almost free.
	source->pump(2);
	QCOMPARE(spy.count(), 0);
}

void TestVideoBroadcaster::restartingAllocatesANewStream() {
	VideoBroadcaster broadcaster;

	QVERIFY(broadcaster.start(std::make_unique< SyntheticVideoSource >(160, 120)));
	const std::uint32_t first = broadcaster.streamID();

	broadcaster.stop();

	QVERIFY(broadcaster.start(std::make_unique< SyntheticVideoSource >(320, 240)));

	// A new id, because the dimensions may have changed and a receiver must not keep painting into the
	// previous picture.
	QVERIFY(broadcaster.streamID() != first);
}

void TestVideoBroadcaster::frameNumbersNeverRepeat() {
	VideoBroadcaster broadcaster;

	auto owned                   = std::make_unique< SyntheticVideoSource >(160, 120);
	SyntheticVideoSource *source = owned.get();

	QVERIFY(broadcaster.start(std::move(owned)));

	QSignalSpy spy(&broadcaster, &VideoBroadcaster::unitReady);

	for (int i = 0; i < 3; ++i) {
		source->pump(static_cast< std::uint64_t >(i));
	}

	QVERIFY(spy.count() >= 3);

	std::uint64_t previous = 0;
	bool first             = true;

	for (int i = 0; i < spy.count(); ++i) {
		const std::uint64_t frame = spy.at(i).at(0).value< Mumble::Protocol::VideoUnitHeader >().frameNumber;

		if (!first) {
			QVERIFY(frame >= previous);
		}

		previous = frame;
		first    = false;
	}

	// Restarting must not rewind the counter: a stale fragment still in flight carrying an old frame
	// number would otherwise be merged into a new frame during reassembly.
	const std::uint64_t highest = previous;

	broadcaster.stop();

	auto second                     = std::make_unique< SyntheticVideoSource >(160, 120);
	SyntheticVideoSource *restarted = second.get();
	QVERIFY(broadcaster.start(std::move(second)));

	QSignalSpy after(&broadcaster, &VideoBroadcaster::unitReady);
	restarted->pump(99);

	QVERIFY(after.count() > 0);
	QVERIFY(after.at(0).at(0).value< Mumble::Protocol::VideoUnitHeader >().frameNumber > highest);
}

void TestVideoBroadcaster::requestingAKeyframeResendsEverything() {
	VideoBroadcaster broadcaster;
	broadcaster.setCodec(1);

	auto owned                   = std::make_unique< SyntheticVideoSource >(320, 240);
	SyntheticVideoSource *source = owned.get();
	source->setChangeRatio(0);

	QVERIFY(broadcaster.start(std::move(owned)));

	QSignalSpy spy(&broadcaster, &VideoBroadcaster::unitReady);

	source->pump(1);
	const qsizetype fullFrame = spy.count();
	QVERIFY(fullFrame > 0);

	spy.clear();
	source->pump(2);
	QCOMPARE(spy.count(), 0);

	// A new subscriber has nothing to build on, so it has to be sent the whole picture again even though
	// nothing changed.
	broadcaster.requestKeyframe();

	spy.clear();
	source->pump(3);
	QCOMPARE(spy.count(), fullFrame);
}

void TestVideoBroadcaster::startingWithNoSourceFails() {
	VideoBroadcaster broadcaster;

	QVERIFY(!broadcaster.start(nullptr));
	QVERIFY(!broadcaster.isActive());
}

void TestVideoBroadcaster::vp8SendsOneWholeFrameUnit() {
	VideoBroadcaster broadcaster;
	// The default. VP8 has no independently decodable sub-frame region, so a frame is exactly one unit
	// covering the whole picture -- the opposite of the tiled codec.
	QCOMPARE(broadcaster.codec(), 0);

	auto owned                   = std::make_unique< SyntheticVideoSource >(320, 240);
	SyntheticVideoSource *source = owned.get();

	QVERIFY(broadcaster.start(std::move(owned)));

	QSignalSpy spy(&broadcaster, &VideoBroadcaster::unitReady);
	source->pump(1);

	QCOMPARE(spy.count(), 1);

	const Mumble::Protocol::VideoUnitHeader header = spy.at(0).at(0).value< Mumble::Protocol::VideoUnitHeader >();

	QCOMPARE(header.x, 0u);
	QCOMPARE(header.y, 0u);
	QCOMPARE(header.width, 320u);
	QCOMPARE(header.height, 240u);
	QVERIFY(header.isKeyframe);
	QVERIFY(header.isFrameEnd);
}

void TestVideoBroadcaster::theCodecCanBeSwitched() {
	VideoBroadcaster broadcaster;

	broadcaster.configure(1, 800, 30, 80, 128);
	QCOMPARE(broadcaster.codec(), 1);

	broadcaster.configure(0, 500, 15, 80, 128);
	QCOMPARE(broadcaster.codec(), 0);

	// Anything that is not the tiled codec means VP8, rather than an out-of-range value silently
	// selecting nothing.
	broadcaster.setCodec(99);
	QCOMPARE(broadcaster.codec(), 0);
}

QTEST_MAIN(TestVideoBroadcaster)
#include "TestVideoBroadcaster.moc"
