// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

// Exercises the whole send-to-receive path in one process: synthetic capture, tiling and JPEG encode,
// fragmentation to MTU-sized packets, reassembly, and decode back to images. Everything except the
// socket itself and the on-screen widget.
//
// The point is to catch the class of bug that unit-testing each stage separately cannot: a producer and
// a consumer that are each self-consistent but disagree with one another.

#include "VideoEncoder.h"
#include "VideoFragmentation.h"
#include "VideoSource.h"

#include <QObject>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtTest>

#include <cstdint>
#include <vector>

using namespace Mumble::Protocol;

namespace {

constexpr std::uint32_t SESSION = 7;
constexpr std::uint32_t STREAM  = 3;

/// Drives every unit of a frame through fragmentation and reassembly, returning what came out the far
/// end. Fails the test if any unit does not survive the round trip.
std::vector< VideoUnit > roundTrip(const std::vector< EncodedVideoUnit > &units, std::uint32_t senderSession) {
	VideoFragmenter fragmenter;
	VideoReassembler reassembler;

	std::vector< VideoUnit > received;

	for (const EncodedVideoUnit &unit : units) {
		VideoUnitHeader header = unit.header;
		header.senderSession   = senderSession;

		if (!fragmenter.fragment(header, unit.payload)) {
			return {};
		}

		VideoUnit assembled;
		bool completed = false;

		for (const std::vector< byte > &packet : fragmenter.packets()) {
			const VideoReassemblyResult result = reassembler.processPacket(packet, senderSession, 1, assembled);

			if (result == VideoReassemblyResult::Complete) {
				completed = true;
			} else if (result == VideoReassemblyResult::Invalid) {
				return {};
			}
		}

		if (!completed) {
			return {};
		}

		received.push_back(std::move(assembled));
	}

	return received;
}

QImage decodeUnit(const VideoUnit &unit) {
	QImage image;
	image.loadFromData(reinterpret_cast< const uchar * >(unit.payload.data()), static_cast< int >(unit.payload.size()),
					   "JPEG");

	return image;
}

} // namespace

class TestVideoPipeline : public QObject {
	Q_OBJECT
private slots:
	void syntheticSourceProducesDistinctFrames();
	void wholeFrameSurvivesTheRoundTrip();
	void decodedTilesMatchTheirHeaders();
	void tilesTogetherCoverTheFrameExactly();
	void unchangedFrameProducesNothing();
	void staticContentSendsOnlyChangedTiles();
	void cameraContentSendsEverything();
	void resetForcesAFullFrame();
	void resizeForcesAFullFrame();
	void frameEndMarksTheLastUnit();
	void oversizeTilesAreRequantisedNotDropped();
	void every128PxTileFitsOneTransportUnit();
};

void TestVideoPipeline::syntheticSourceProducesDistinctFrames() {
	SyntheticVideoSource source(320, 240);
	QVERIFY(source.start());
	QVERIFY(source.isRunning());

	const QImage first  = source.render(0);
	const QImage second = source.render(1);

	QCOMPARE(first.size(), QSize(320, 240));
	QVERIFY(!first.isNull());
	// Consecutive frames must differ, or the dirty-tile tests below would pass vacuously.
	QVERIFY(first != second);
}

void TestVideoPipeline::wholeFrameSurvivesTheRoundTrip() {
	SyntheticVideoSource source(640, 480);
	TiledImageEncoder encoder;

	const QImage frame                          = source.render(0);
	const std::vector< EncodedVideoUnit > units = encoder.encode(frame, STREAM, 1, 123456);

	QVERIFY(!units.empty());
	QCOMPARE(encoder.lastStats().tilesDroppedOversize, 0u);

	const std::vector< VideoUnit > received = roundTrip(units, SESSION);

	QCOMPARE(received.size(), units.size());

	for (std::size_t i = 0; i < received.size(); ++i) {
		QCOMPARE(received[i].payload, units[i].payload);
		QCOMPARE(received[i].header.unitID, units[i].header.unitID);
		QCOMPARE(received[i].header.streamID, STREAM);
		QCOMPARE(received[i].header.senderSession, SESSION);
	}
}

void TestVideoPipeline::decodedTilesMatchTheirHeaders() {
	SyntheticVideoSource source(640, 480);
	TiledImageEncoder encoder;

	const std::vector< VideoUnit > received = roundTrip(encoder.encode(source.render(0), STREAM, 1, 1), SESSION);

	QVERIFY(!received.empty());

	for (const VideoUnit &unit : received) {
		const QImage tile = decodeUnit(unit);

		// The geometry a receiver uses to place the tile has to agree with what actually decodes, or the
		// picture is assembled wrong in a way no amount of transport testing would reveal.
		QVERIFY(!tile.isNull());
		QCOMPARE(static_cast< std::uint32_t >(tile.width()), unit.header.width);
		QCOMPARE(static_cast< std::uint32_t >(tile.height()), unit.header.height);
	}
}

void TestVideoPipeline::tilesTogetherCoverTheFrameExactly() {
	// 700x460 is deliberately not a multiple of the tile size, so the right and bottom edges are
	// partial tiles.
	SyntheticVideoSource source(700, 460);
	TiledImageEncoder encoder;

	const std::vector< VideoUnit > received = roundTrip(encoder.encode(source.render(0), STREAM, 1, 1), SESSION);

	QVERIFY(!received.empty());

	QImage canvas(700, 460, QImage::Format_RGB32);
	canvas.fill(Qt::black);

	std::size_t covered = 0;

	for (const VideoUnit &unit : received) {
		const QImage tile = decodeUnit(unit);
		QVERIFY(!tile.isNull());

		QVERIFY(unit.header.x + unit.header.width <= 700);
		QVERIFY(unit.header.y + unit.header.height <= 460);

		covered += static_cast< std::size_t >(unit.header.width) * unit.header.height;

		QPainter painter(&canvas);
		painter.drawImage(static_cast< int >(unit.header.x), static_cast< int >(unit.header.y), tile);
	}

	// Every pixel of the frame is accounted for exactly once.
	QCOMPARE(covered, static_cast< std::size_t >(700) * 460);
}

void TestVideoPipeline::unchangedFrameProducesNothing() {
	SyntheticVideoSource source(640, 480);
	TiledImageEncoder encoder;

	const QImage frame = source.render(0);

	QVERIFY(!encoder.encode(frame, STREAM, 1, 1).empty());

	// The identical frame again is a no-op: the receiver's picture is already correct.
	const std::vector< EncodedVideoUnit > second = encoder.encode(frame, STREAM, 2, 2);

	QVERIFY(second.empty());
	QCOMPARE(encoder.lastStats().tilesEncoded, 0u);
	QVERIFY(encoder.lastStats().tilesUnchanged > 0);
}

void TestVideoPipeline::staticContentSendsOnlyChangedTiles() {
	SyntheticVideoSource source(640, 480);
	source.setChangeRatio(10);

	TiledImageEncoder encoder;

	const std::vector< EncodedVideoUnit > first = encoder.encode(source.render(0), STREAM, 1, 1);
	QVERIFY(!first.empty());

	const std::vector< EncodedVideoUnit > second = encoder.encode(source.render(1), STREAM, 2, 2);

	// This is the property that makes screen sharing affordable at all.
	QVERIFY(!second.empty());
	QVERIFY(second.size() < first.size());
	QVERIFY(encoder.lastStats().tilesUnchanged > 0);
}

void TestVideoPipeline::cameraContentSendsEverything() {
	SyntheticVideoSource source(320, 240);
	source.setChangeRatio(100);

	TiledImageEncoder encoder;

	encoder.encode(source.render(0), STREAM, 1, 1);
	const std::vector< EncodedVideoUnit > second = encoder.encode(source.render(1), STREAM, 2, 2);

	// Camera-like content defeats dirty-tile detection, which is expected rather than a fault.
	QVERIFY(!second.empty());
	QCOMPARE(encoder.lastStats().tilesUnchanged, 0u);
}

void TestVideoPipeline::resetForcesAFullFrame() {
	SyntheticVideoSource source(320, 240);
	TiledImageEncoder encoder;

	const QImage frame = source.render(0);

	const std::size_t full = encoder.encode(frame, STREAM, 1, 1).size();
	QVERIFY(encoder.encode(frame, STREAM, 2, 2).empty());

	encoder.reset();

	QCOMPARE(encoder.encode(frame, STREAM, 3, 3).size(), full);
}

void TestVideoPipeline::resizeForcesAFullFrame() {
	TiledImageEncoder encoder;

	SyntheticVideoSource small(320, 240);
	SyntheticVideoSource large(640, 480);

	QVERIFY(!encoder.encode(small.render(0), STREAM, 1, 1).empty());

	// A different frame size means the tile grid no longer lines up with the stored hashes.
	const std::vector< EncodedVideoUnit > resized = encoder.encode(large.render(0), STREAM, 2, 2);

	QVERIFY(!resized.empty());
	QCOMPARE(encoder.lastStats().tilesUnchanged, 0u);
}

void TestVideoPipeline::frameEndMarksTheLastUnit() {
	SyntheticVideoSource source(640, 480);
	TiledImageEncoder encoder;

	const std::vector< EncodedVideoUnit > units = encoder.encode(source.render(0), STREAM, 1, 1);

	QVERIFY(units.size() > 1);

	for (std::size_t i = 0; i + 1 < units.size(); ++i) {
		QVERIFY(!units[i].header.isFrameEnd);
	}

	QVERIFY(units.back().header.isFrameEnd);

	// And it survives the wire.
	const std::vector< VideoUnit > received = roundTrip(units, SESSION);
	QCOMPARE(received.size(), units.size());
	QVERIFY(received.back().header.isFrameEnd);
}

void TestVideoPipeline::oversizeTilesAreRequantisedNotDropped() {
	// Swept rather than pinned to one tile size, because the size at which incompressible content stops
	// fitting at full quality is a property of the transport budget and the JPEG encoder, not something a
	// test should hardcode. What must hold at every size is that nothing is ever emitted too large and
	// nothing ever disappears unaccounted for.
	bool sawRequantise = false;

	for (int size : { 128, 192, 256, 384, 512, 768, 1024 }) {
		SyntheticVideoSource source(size, size);
		source.setChangeRatio(100);

		TiledImageEncoder encoder;
		encoder.setTileSize(size);
		encoder.setQuality(100);

		const std::vector< EncodedVideoUnit > units = encoder.encode(source.render(0), STREAM, 1, 1);
		const TiledImageEncoder::Stats stats        = encoder.lastStats();

		QCOMPARE(stats.tilesConsidered, 1u);
		// One top-level tile can now become several units if it had to be split, so the old "exactly one
		// bucket per tile" bookkeeping no longer applies - what still has to hold is that every emitted
		// unit is accounted for in tilesEncoded, and every unit returned is one of them.
		QCOMPARE(units.size(), static_cast< std::size_t >(stats.tilesEncoded));

		for (const EncodedVideoUnit &unit : units) {
			QVERIFY(unit.payload.size() <= VideoFragmenter::maxUnitSize());
		}

		QCOMPARE(roundTrip(units, SESSION).size(), units.size());

		// A tile too noisy to fit whole even at the quality floor is split into independently-encoded
		// quadrants, recursively, down to a 16px floor - and pure noise at 16px compresses to a few dozen
		// bytes at worst, nowhere near the >50KB budget maxUnitSize() actually allows. So across this
		// entire size sweep, splitting should always finish the job: the fallback this test exists to
		// exercise never has to fall back all the way to dropping content on the floor.
		QCOMPARE(stats.tilesDroppedOversize, 0u);
		QVERIFY(!units.empty());

		if (stats.tilesRequantised > 0) {
			sawRequantise = true;
		}
	}

	// Graceful degradation has to actually happen somewhere in that range, or the retry loop is dead
	// code that a future change could break unnoticed.
	QVERIFY(sawRequantise);
}

void TestVideoPipeline::every128PxTileFitsOneTransportUnit() {
	// The measurement that set the tile-size default: at 128px no tile of realistic content needs
	// requantising, so quality is never silently degraded in normal operation.
	SyntheticVideoSource source(1920, 1080);
	source.setChangeRatio(100);

	TiledImageEncoder encoder;
	encoder.setTileSize(128);
	encoder.setQuality(80);

	const std::vector< EncodedVideoUnit > units = encoder.encode(source.render(0), STREAM, 1, 1);

	QVERIFY(!units.empty());
	QCOMPARE(encoder.lastStats().tilesDroppedOversize, 0u);
	QCOMPARE(encoder.lastStats().tilesRequantised, 0u);

	for (const EncodedVideoUnit &unit : units) {
		QVERIFY(unit.payload.size() <= VideoFragmenter::maxUnitSize());
	}
}

QTEST_MAIN(TestVideoPipeline)
#include "TestVideoPipeline.moc"
