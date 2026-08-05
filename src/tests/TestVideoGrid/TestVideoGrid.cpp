// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

// The last hop: tiles produced by the real encoder, painted into the real display surface.
//
// Together with TestVideoPipeline this closes the loop from a captured frame to what a viewer actually
// sees, so a mistake in tile placement shows up as a wrong picture here rather than as a bug report.

#include "Mumble.pb.h"
#include "VP8Codec.h"
#include "VideoEncoder.h"
#include "VideoGrid.h"
#include "VideoSource.h"

#include <QObject>
#include <QSignalSpy>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtTest>

#include <cstdint>
#include <vector>

namespace {

constexpr unsigned int SENDER = 5;
constexpr unsigned int STREAM = 1;

/// Announces a stream, as the VideoState message does. Nothing is decoded without this: the payload
/// never says which codec produced it, so an unannounced stream has no decoder to hand it to.
void announce(VideoGrid &grid, unsigned int sender, unsigned int stream,
			  int codec = MumbleProto::VideoState_Codec_TiledImage) {
	grid.setStreamCodec(sender, stream, codec);
}

/// Feeds every tile of one encoded frame into the grid, as the network path would.
void deliverFrame(VideoGrid &grid, const std::vector< EncodedVideoUnit > &units, unsigned int sender,
				  unsigned int stream, int codec = MumbleProto::VideoState_Codec_TiledImage) {
	announce(grid, sender, stream, codec);


	for (const EncodedVideoUnit &unit : units) {
		grid.onVideoUnitReceived(
			sender, stream, unit.header.x, unit.header.y,
			QByteArray(reinterpret_cast< const char * >(unit.payload.data()), static_cast< int >(unit.payload.size())));
	}
}

/// Mean absolute per-channel difference between two images of the same size, 0-255.
double meanDifference(const QImage &a, const QImage &b) {
	if (a.size() != b.size() || a.isNull()) {
		return 255.0;
	}

	const QImage left  = a.convertToFormat(QImage::Format_RGB32);
	const QImage right = b.convertToFormat(QImage::Format_RGB32);

	double total = 0.0;

	for (int y = 0; y < left.height(); ++y) {
		for (int x = 0; x < left.width(); ++x) {
			const QRgb p = left.pixel(x, y);
			const QRgb q = right.pixel(x, y);

			total += std::abs(qRed(p) - qRed(q)) + std::abs(qGreen(p) - qGreen(q)) + std::abs(qBlue(p) - qBlue(q));
		}
	}

	return total / (left.width() * left.height() * 3.0);
}

} // namespace

class TestVideoGrid : public QObject {
	Q_OBJECT
private slots:
	void anEncodedFrameIsReassembledIntoThePicture();
	void aPartialFrameShowsWhatArrived();
	void surfaceGrowthKeepsWhatWasAlreadyDrawn();
	void aNewStreamFromTheSameSenderStartsFresh();
	void sendersAppearAndDisappear();
	void theSenderCountIsCapped();
	void tilesOutsideTheSurfaceBoundAreRefused();
	void aVp8StreamIsDecoded();
	void unitsForAnUnannouncedStreamAreDropped();
	void anUnknownCodecIsDropped();
	void aSenderKeepsItsNameAcrossANewStream();
	void aStuckDecoderAsksForAKeyframe();
	void anAnnouncedButBlankStreamPaintsWithoutCrashing();
	void nonJpegDataIsRefused();
	void itPaintsWithoutCrashing();
};

void TestVideoGrid::anEncodedFrameIsReassembledIntoThePicture() {
	SyntheticVideoSource source(640, 480);
	// A smooth gradient rather than the default noise. Noise is incompressible, so JPEG mangles it and a
	// fidelity comparison would measure the codec rather than the tiling. A gradient varies across both
	// axes, so a tile drawn at the wrong offset still shows up plainly, while quantisation error stays
	// small enough for the threshold below to mean something.
	source.setChangeRatio(0);

	TiledImageEncoder encoder;

	const QImage original = source.render(0);

	VideoGrid grid;
	deliverFrame(grid, encoder.encode(original, STREAM, 1, 1), SENDER, STREAM);

	const QImage shown = grid.surfaceFor(SENDER);

	QCOMPARE(shown.size(), original.size());

	// JPEG is lossy, so this cannot be an exact comparison. A mean difference of a level or two is normal
	// for quality 80 on smooth content; a tile placed at the wrong offset runs into the tens.
	const double difference = meanDifference(original, shown);

	QVERIFY2(difference < 4.0, qPrintable(QStringLiteral("mean difference %1").arg(difference)));

	// And prove the threshold is actually discriminating, or this test would pass whatever the grid drew.
	// Mirroring rather than moving a single tile: the gradient varies along x, so a mirror changes almost
	// every pixel, whereas displacing one 128x128 tile touches only five percent of a 640x480 frame and
	// barely moves the mean at all.
	QVERIFY(meanDifference(original, shown.flipped(Qt::Horizontal)) > 4.0);
}

void TestVideoGrid::aPartialFrameShowsWhatArrived() {
	SyntheticVideoSource source(640, 480);
	TiledImageEncoder encoder;

	std::vector< EncodedVideoUnit > units = encoder.encode(source.render(0), STREAM, 1, 1);
	QVERIFY(units.size() > 4);

	// Drop half the tiles, as loss would. The surface must still show the ones that made it: this is the
	// property that makes independently decodable tiles worth the overhead.
	units.resize(units.size() / 2);

	VideoGrid grid;
	deliverFrame(grid, units, SENDER, STREAM);

	const QImage shown = grid.surfaceFor(SENDER);
	QVERIFY(!shown.isNull());

	// Something was drawn, rather than the surface staying blank waiting for a complete frame.
	bool anyNonBlack = false;

	for (int y = 0; y < shown.height() && !anyNonBlack; ++y) {
		for (int x = 0; x < shown.width(); ++x) {
			if (shown.pixel(x, y) != qRgb(0, 0, 0)) {
				anyNonBlack = true;
				break;
			}
		}
	}

	QVERIFY(anyNonBlack);
}

void TestVideoGrid::surfaceGrowthKeepsWhatWasAlreadyDrawn() {
	VideoGrid grid;

	QImage first(64, 64, QImage::Format_RGB32);
	first.fill(QColor(200, 30, 40));

	QByteArray encoded;
	QBuffer buffer(&encoded);
	buffer.open(QIODevice::WriteOnly);
	QVERIFY(first.save(&buffer, "JPEG", 95));
	buffer.close();

	announce(grid, SENDER, STREAM);
	grid.onVideoUnitReceived(SENDER, STREAM, 0, 0, encoded);
	QCOMPARE(grid.surfaceFor(SENDER).size(), QSize(64, 64));

	// A tile further right and down forces the surface to grow.
	grid.onVideoUnitReceived(SENDER, STREAM, 64, 64, encoded);
	QCOMPARE(grid.surfaceFor(SENDER).size(), QSize(128, 128));

	// And the first tile is still there. Reallocating blank would make a resolution change flash black.
	const QRgb corner = grid.surfaceFor(SENDER).pixel(10, 10);
	QVERIFY(qRed(corner) > 150);
	QVERIFY(qGreen(corner) < 100);
}

void TestVideoGrid::aNewStreamFromTheSameSenderStartsFresh() {
	VideoGrid grid;

	QImage big(128, 128, QImage::Format_RGB32);
	big.fill(Qt::white);

	QByteArray encoded;
	QBuffer buffer(&encoded);
	buffer.open(QIODevice::WriteOnly);
	QVERIFY(big.save(&buffer, "JPEG", 90));
	buffer.close();

	announce(grid, SENDER, 1);
	grid.onVideoUnitReceived(SENDER, 1, 0, 0, encoded);
	QCOMPARE(grid.surfaceFor(SENDER).size(), QSize(128, 128));

	// A different stream id means a different source or different dimensions, so the old picture is not
	// part of the same image any more.
	QImage small(32, 32, QImage::Format_RGB32);
	small.fill(Qt::white);

	QByteArray smallEncoded;
	QBuffer smallBuffer(&smallEncoded);
	smallBuffer.open(QIODevice::WriteOnly);
	QVERIFY(small.save(&smallBuffer, "JPEG", 90));
	smallBuffer.close();

	announce(grid, SENDER, 2);
	grid.onVideoUnitReceived(SENDER, 2, 0, 0, smallEncoded);
	QCOMPARE(grid.surfaceFor(SENDER).size(), QSize(32, 32));
}

void TestVideoGrid::sendersAppearAndDisappear() {
	VideoGrid grid;
	QSignalSpy spy(&grid, &VideoGrid::senderCountChanged);

	QImage tile(32, 32, QImage::Format_RGB32);
	tile.fill(Qt::green);

	QByteArray encoded;
	QBuffer buffer(&encoded);
	buffer.open(QIODevice::WriteOnly);
	QVERIFY(tile.save(&buffer, "JPEG", 90));
	buffer.close();

	announce(grid, 1, STREAM);
	announce(grid, 2, STREAM);
	grid.onVideoUnitReceived(1, STREAM, 0, 0, encoded);
	grid.onVideoUnitReceived(2, STREAM, 0, 0, encoded);
	QCOMPARE(grid.senderCount(), 2);
	QCOMPARE(spy.count(), 2);

	// Further tiles from a known sender are not a new arrival.
	grid.onVideoUnitReceived(1, STREAM, 0, 0, encoded);
	QCOMPARE(spy.count(), 2);

	grid.removeSender(1);
	QCOMPARE(grid.senderCount(), 1);
	QCOMPARE(spy.count(), 3);

	// Removing someone who is not there changes nothing.
	grid.removeSender(99);
	QCOMPARE(spy.count(), 3);

	grid.clear();
	QCOMPARE(grid.senderCount(), 0);
	QCOMPARE(spy.count(), 4);
}

void TestVideoGrid::theSenderCountIsCapped() {
	VideoGrid grid;

	QImage tile(16, 16, QImage::Format_RGB32);
	tile.fill(Qt::blue);

	QByteArray encoded;
	QBuffer buffer(&encoded);
	buffer.open(QIODevice::WriteOnly);
	QVERIFY(tile.save(&buffer, "JPEG", 90));
	buffer.close();

	for (unsigned int session = 1; session <= VideoGrid::MAX_SENDERS + 10; ++session) {
		announce(grid, session, STREAM);
		grid.onVideoUnitReceived(session, STREAM, 0, 0, encoded);
	}

	QCOMPARE(grid.senderCount(), VideoGrid::MAX_SENDERS);
}

// The bug this file shipped: every camera stream uses VP8, the grid only ever called the JPEG decoder,
// and so a viewer saw nothing at all while packets arrived normally. Nothing here covered a codec other
// than the default, so nothing failed.
void TestVideoGrid::aVp8StreamIsDecoded() {
	// Not checking isValid() here: the encoder builds its context on the first frame, once it knows the
	// resolution, so it is legitimately invalid until then.
	VP8Encoder encoder;

	encoder.setBitrate(600);
	encoder.setFramerate(30);

	SyntheticVideoSource source(128, 128);
	source.setChangeRatio(100);

	const QImage frame = source.render(0);
	QVERIFY(!frame.isNull());

	const std::vector< EncodedVideoUnit > units = encoder.encode(frame, STREAM, 0, 0, true);
	QVERIFY(!units.empty());

	VideoGrid grid;
	deliverFrame(grid, units, SENDER, STREAM, MumbleProto::VideoState_Codec_VP8);

	QCOMPARE(grid.senderCount(), 1);
	QCOMPARE(grid.surfaceFor(SENDER).size(), QSize(128, 128));
}

// Units must never select their own decoder: the payload comes from another client, so trusting it to
// say what it is would make every decoder in the client reachable by anyone who can send a packet.
void TestVideoGrid::unitsForAnUnannouncedStreamAreDropped() {
	VideoGrid grid;

	QImage tile(32, 32, QImage::Format_RGB32);
	tile.fill(Qt::green);

	QByteArray jpeg;
	QBuffer buffer(&jpeg);
	buffer.open(QIODevice::WriteOnly);
	QVERIFY(tile.save(&buffer, "JPEG"));

	// Perfectly valid JPEG, but no announcement arrived for this stream.
	grid.onVideoUnitReceived(SENDER, STREAM, 0, 0, jpeg);
	QCOMPARE(grid.senderCount(), 0);

	// And it starts working the moment the announcement does arrive.
	announce(grid, SENDER, STREAM);
	grid.onVideoUnitReceived(SENDER, STREAM, 0, 0, jpeg);
	QCOMPARE(grid.senderCount(), 1);
}

void TestVideoGrid::anUnknownCodecIsDropped() {
	VideoGrid grid;

	QImage tile(32, 32, QImage::Format_RGB32);
	tile.fill(Qt::green);

	QByteArray jpeg;
	QBuffer buffer(&jpeg);
	buffer.open(QIODevice::WriteOnly);
	QVERIFY(tile.save(&buffer, "JPEG"));

	// CODEC_UNKNOWN is what a codec added after this build resolves to. Dropped, not guessed at, even
	// though these particular bytes would decode.
	announce(grid, SENDER, STREAM, MumbleProto::VideoState_Codec_CODEC_UNKNOWN);
	grid.onVideoUnitReceived(SENDER, STREAM, 0, 0, jpeg);
	QCOMPARE(grid.senderCount(), 0);
}

// A receiver holds a surface from the announcement onward, before anything has decoded into it. If it
// is not sharing its own camera either, there is a window where a surface exists but nothing is drawable
// - and the grid still has to survive being painted in it.
void TestVideoGrid::anAnnouncedButBlankStreamPaintsWithoutCrashing() {
	VideoGrid grid;
	grid.resize(320, 240);

	announce(grid, SENDER, STREAM);

	QCOMPARE(grid.senderCount(), 0);

	QImage target(320, 240, QImage::Format_RGB32);
	grid.render(&target);
}

// Every tile except your own went unlabelled, because the paint code had a name for the self view and
// an empty string for everyone else.
void TestVideoGrid::aSenderKeepsItsNameAcrossANewStream() {
	VideoGrid grid;

	// No surface yet, so there is nothing to name.
	grid.setSenderName(SENDER, QStringLiteral("alice"));
	QCOMPARE(grid.senderName(SENDER), QString());

	announce(grid, SENDER, STREAM);
	grid.setSenderName(SENDER, QStringLiteral("alice"));
	QCOMPARE(grid.senderName(SENDER), QStringLiteral("alice"));

	// A new stream is the same person: the picture is discarded, the name is not.
	announce(grid, SENDER, STREAM + 1);
	QCOMPARE(grid.senderName(SENDER), QStringLiteral("alice"));

	grid.setSenderName(SENDER, QStringLiteral("alice-renamed"));
	QCOMPARE(grid.senderName(SENDER), QStringLiteral("alice-renamed"));

	grid.removeSender(SENDER);
	QCOMPARE(grid.senderName(SENDER), QString());
}

// After lost reference frames a VP8 decoder fails on every unit until a keyframe arrives, so a run of
// failures is the signal that waiting is pointless. The grid reports it; sending the actual request is
// its owner's job, since the grid has no connection.
void TestVideoGrid::aStuckDecoderAsksForAKeyframe() {
	VideoGrid grid;
	QSignalSpy needed(&grid, &VideoGrid::keyframeNeeded);

	announce(grid, SENDER, STREAM, MumbleProto::VideoState_Codec_VP8);

	const QByteArray garbage("no decoder will make sense of this");

	for (int i = 0; i < VideoGrid::KEYFRAME_REQUEST_AFTER_FAILURES - 1; ++i) {
		grid.onVideoUnitReceived(SENDER, STREAM, 0, 0, garbage);
	}

	QCOMPARE(needed.count(), 0);

	grid.onVideoUnitReceived(SENDER, STREAM, 0, 0, garbage);
	QCOMPARE(needed.count(), 1);
	QCOMPARE(needed.at(0).at(0).toUInt(), SENDER);
	QCOMPARE(needed.at(0).at(1).toUInt(), STREAM);

	// The counter restarts after each report, so a sender that never answers is asked again only after
	// another full run - not on every subsequent unit.
	for (int i = 0; i < VideoGrid::KEYFRAME_REQUEST_AFTER_FAILURES - 1; ++i) {
		grid.onVideoUnitReceived(SENDER, STREAM, 0, 0, garbage);
	}

	QCOMPARE(needed.count(), 1);

	// A stream announced with a codec this build cannot decode must never generate requests: they would
	// ask for something no keyframe can fix.
	VideoGrid unknowing;
	QSignalSpy futile(&unknowing, &VideoGrid::keyframeNeeded);

	announce(unknowing, SENDER, STREAM, MumbleProto::VideoState_Codec_CODEC_UNKNOWN);

	for (int i = 0; i < 3 * VideoGrid::KEYFRAME_REQUEST_AFTER_FAILURES; ++i) {
		unknowing.onVideoUnitReceived(SENDER, STREAM, 0, 0, garbage);
	}

	QCOMPARE(futile.count(), 0);
}

void TestVideoGrid::tilesOutsideTheSurfaceBoundAreRefused() {
	VideoGrid grid;

	QImage tile(16, 16, QImage::Format_RGB32);
	tile.fill(Qt::red);

	QByteArray encoded;
	QBuffer buffer(&encoded);
	buffer.open(QIODevice::WriteOnly);
	QVERIFY(tile.save(&buffer, "JPEG", 90));
	buffer.close();

	// The offset comes off the network. A tile claiming to belong far outside any real frame must not
	// make the client allocate a surface to match.
	announce(grid, SENDER, STREAM);
	grid.onVideoUnitReceived(SENDER, STREAM, 100000, 100000, encoded);

	QCOMPARE(grid.senderCount(), 0);
	QVERIFY(grid.surfaceFor(SENDER).isNull());

	// Exactly at the boundary is still refused, since the tile would extend past it.
	grid.onVideoUnitReceived(SENDER, STREAM, static_cast< unsigned int >(VideoGrid::MAX_SURFACE_WIDTH), 0, encoded);
	QCOMPARE(grid.senderCount(), 0);
}

void TestVideoGrid::nonJpegDataIsRefused() {
	VideoGrid grid;

	announce(grid, SENDER, STREAM);
	grid.onVideoUnitReceived(SENDER, STREAM, 0, 0, QByteArray("not an image at all"));
	QCOMPARE(grid.senderCount(), 0);

	grid.onVideoUnitReceived(SENDER, STREAM, 0, 0, QByteArray());
	QCOMPARE(grid.senderCount(), 0);

	// A PNG is a valid image but not what the codec says it is. Decoding is pinned to JPEG so that a
	// sender cannot choose which of Qt's decoders to hand its bytes to.
	QImage png(16, 16, QImage::Format_RGB32);
	png.fill(Qt::yellow);

	QByteArray encoded;
	QBuffer buffer(&encoded);
	buffer.open(QIODevice::WriteOnly);
	QVERIFY(png.save(&buffer, "PNG"));
	buffer.close();

	grid.onVideoUnitReceived(SENDER, STREAM, 0, 0, encoded);
	QCOMPARE(grid.senderCount(), 0);
}

void TestVideoGrid::itPaintsWithoutCrashing() {
	SyntheticVideoSource source(320, 240);
	TiledImageEncoder encoder;

	VideoGrid grid;
	grid.resize(800, 600);

	for (unsigned int sender = 1; sender <= 5; ++sender) {
		deliverFrame(grid, encoder.encode(source.render(sender), STREAM, sender, 1), sender, STREAM);
		encoder.reset();
	}

	QCOMPARE(grid.senderCount(), 5);

	// Render into an image rather than showing a window, so this works on a headless machine.
	QImage target(800, 600, QImage::Format_RGB32);
	grid.render(&target);

	// The grid drew something other than its background.
	bool anyNonBlack = false;

	for (int y = 0; y < target.height() && !anyNonBlack; y += 4) {
		for (int x = 0; x < target.width(); x += 4) {
			if (target.pixel(x, y) != qRgb(0, 0, 0)) {
				anyNonBlack = true;
				break;
			}
		}
	}

	QVERIFY(anyNonBlack);
}

QTEST_MAIN(TestVideoGrid)
#include "TestVideoGrid.moc"
