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
			  int codec = MumbleProto::VideoState_Codec_TiledImage,
			  int sourceKind = MumbleProto::VideoState_SourceKind_SOURCE_UNKNOWN) {
	grid.setStreamCodec(sender, stream, sourceKind, codec);
}

/// Feeds every tile of one encoded frame into the grid, as the network path would.
void deliverFrame(VideoGrid &grid, const std::vector< EncodedVideoUnit > &units, unsigned int sender,
				  unsigned int stream, int codec = MumbleProto::VideoState_Codec_TiledImage) {
	announce(grid, sender, stream, codec);


	for (const EncodedVideoUnit &unit : units) {
		grid.onVideoUnitReceived(
			sender, stream, unit.header.frameNumber, unit.header.isKeyframe, unit.header.x, unit.header.y,
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
	void twoStreamsFromTheSameSenderCoexist();
	void aRestartedStreamOfTheSameKindReplacesTheOld();
	void reannouncingAStreamDiscardsItsOldPicture();
	void sendersAppearAndDisappear();
	void theSenderCountIsCapped();
	void tilesOutsideTheSurfaceBoundAreRefused();
	void aVp8StreamIsDecoded();
	void unitsForAnUnannouncedStreamAreDropped();
	void anUnknownCodecIsDropped();
	void aSenderKeepsItsNameAcrossANewStream();
	void aStuckDecoderAsksForAKeyframe();
	void aGapInVp8FramesFreezesInsteadOfCorrupting();
	void decodeResumesAtTheNextKeyframe();
	void aStaleVp8FrameArrivingLateIsDropped();
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

	const QImage shown = grid.surfaceFor(SENDER, STREAM);

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

	const QImage shown = grid.surfaceFor(SENDER, STREAM);
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
	grid.onVideoUnitReceived(SENDER, STREAM, 0, true, 0, 0, encoded);
	QCOMPARE(grid.surfaceFor(SENDER, STREAM).size(), QSize(64, 64));

	// A tile further right and down forces the surface to grow.
	grid.onVideoUnitReceived(SENDER, STREAM, 0, true, 64, 64, encoded);
	QCOMPARE(grid.surfaceFor(SENDER, STREAM).size(), QSize(128, 128));

	// And the first tile is still there. Reallocating blank would make a resolution change flash black.
	const QRgb corner = grid.surfaceFor(SENDER, STREAM).pixel(10, 10);
	QVERIFY(qRed(corner) > 150);
	QVERIFY(qGreen(corner) < 100);
}

// Regression test for the bug this class shipped with: a second VideoState from a sender who already had
// a surface replaced it wholesale, so a camera and a screen from one person could never both be shown -
// whichever was announced second silently ate the first. Two different stream ids from one sender must
// now produce two independent surfaces, neither disturbing the other.
void TestVideoGrid::twoStreamsFromTheSameSenderCoexist() {
	VideoGrid grid;

	QImage big(128, 128, QImage::Format_RGB32);
	big.fill(Qt::white);

	QByteArray encoded;
	QBuffer buffer(&encoded);
	buffer.open(QIODevice::WriteOnly);
	QVERIFY(big.save(&buffer, "JPEG", 90));
	buffer.close();

	announce(grid, SENDER, 1, MumbleProto::VideoState_Codec_TiledImage, MumbleProto::VideoState_SourceKind_Camera);
	grid.onVideoUnitReceived(SENDER, 1, 0, true, 0, 0, encoded);
	QCOMPARE(grid.surfaceFor(SENDER, 1).size(), QSize(128, 128));

	QImage small(32, 32, QImage::Format_RGB32);
	small.fill(Qt::white);

	QByteArray smallEncoded;
	QBuffer smallBuffer(&smallEncoded);
	smallBuffer.open(QIODevice::WriteOnly);
	QVERIFY(small.save(&smallBuffer, "JPEG", 90));
	smallBuffer.close();

	// A second stream id from the same sender - camera plus screen, in practice. Of a different kind:
	// a second stream of the SAME kind is a restart and replaces the first (see the next test).
	announce(grid, SENDER, 2, MumbleProto::VideoState_Codec_TiledImage, MumbleProto::VideoState_SourceKind_Display);
	grid.onVideoUnitReceived(SENDER, 2, 0, true, 0, 0, smallEncoded);
	QCOMPARE(grid.surfaceFor(SENDER, 2).size(), QSize(32, 32));

	// The first stream's picture is untouched by the second one arriving.
	QCOMPARE(grid.surfaceFor(SENDER, 1).size(), QSize(128, 128));

	// Both count toward what gets drawn: this is two tiles, not one replacing the other.
	QCOMPARE(grid.senderCount(), 2);

	// Ending just the second stream leaves the first exactly as it was.
	grid.removeSender(SENDER, 2);
	QVERIFY(grid.surfaceFor(SENDER, 2).isNull());
	QCOMPARE(grid.surfaceFor(SENDER, 1).size(), QSize(128, 128));
	QCOMPARE(grid.senderCount(), 1);
}

// Re-announcing the *same* stream id with a new codec discards whatever was drawn under it: the protocol
// guarantees a stream id is reused only when nothing about its content is still valid.
void TestVideoGrid::reannouncingAStreamDiscardsItsOldPicture() {
	VideoGrid grid;

	QImage big(128, 128, QImage::Format_RGB32);
	big.fill(Qt::white);

	QByteArray encoded;
	QBuffer buffer(&encoded);
	buffer.open(QIODevice::WriteOnly);
	QVERIFY(big.save(&buffer, "JPEG", 90));
	buffer.close();

	announce(grid, SENDER, STREAM);
	grid.onVideoUnitReceived(SENDER, STREAM, 0, true, 0, 0, encoded);
	QCOMPARE(grid.surfaceFor(SENDER, STREAM).size(), QSize(128, 128));

	// Same stream id, announced again with a different codec - a misbehaving peer, since the protocol
	// says this id should have changed, but the grid must not go on treating stale tiles as current.
	announce(grid, SENDER, STREAM, MumbleProto::VideoState_Codec_VP8);
	QVERIFY(grid.surfaceFor(SENDER, STREAM).isNull());
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
	grid.onVideoUnitReceived(1, STREAM, 0, true, 0, 0, encoded);
	grid.onVideoUnitReceived(2, STREAM, 0, true, 0, 0, encoded);
	QCOMPARE(grid.senderCount(), 2);
	QCOMPARE(spy.count(), 2);

	// Further tiles from a known sender are not a new arrival.
	grid.onVideoUnitReceived(1, STREAM, 0, true, 0, 0, encoded);
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
		grid.onVideoUnitReceived(session, STREAM, 0, true, 0, 0, encoded);
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
	QCOMPARE(grid.surfaceFor(SENDER, STREAM).size(), QSize(128, 128));
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
	grid.onVideoUnitReceived(SENDER, STREAM, 0, true, 0, 0, jpeg);
	QCOMPARE(grid.senderCount(), 0);

	// And it starts working the moment the announcement does arrive.
	announce(grid, SENDER, STREAM);
	grid.onVideoUnitReceived(SENDER, STREAM, 0, true, 0, 0, jpeg);
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
	grid.onVideoUnitReceived(SENDER, STREAM, 0, true, 0, 0, jpeg);
	QCOMPARE(grid.senderCount(), 0);
}

// A receiver holds a surface from the announcement onward, before anything has decoded into it. If it
// is not sharing its own camera either, there is a window where a surface exists but nothing is drawable
// - and the grid still has to survive being painted in it.
// The green-frame bug: VP8 decodes an inter-frame whose reference was lost into a plausible corrupted
// image rather than failing, so the failure-counting recovery path never noticed anything was wrong.
// The grid now enforces frame continuity itself: a gap freezes the picture on the last good frame and
// asks for a keyframe immediately - once, not per dropped unit.
void TestVideoGrid::aGapInVp8FramesFreezesInsteadOfCorrupting() {
	VP8Encoder encoder;
	encoder.setBitrate(600);
	encoder.setFramerate(30);

	SyntheticVideoSource source(128, 128);
	source.setChangeRatio(100);

	VideoGrid grid;
	QSignalSpy needed(&grid, &VideoGrid::keyframeNeeded);

	announce(grid, SENDER, STREAM, MumbleProto::VideoState_Codec_VP8);

	// Frames 0 (keyframe), 1, 2 encoded against each other; the wire loses frame 1.
	const auto f0 = encoder.encode(source.render(0), STREAM, 0, 0, true);
	const auto f1 = encoder.encode(source.render(1), STREAM, 1, 1, false);
	const auto f2 = encoder.encode(source.render(2), STREAM, 2, 2, false);
	QVERIFY(!f0.empty() && !f1.empty() && !f2.empty());

	deliverFrame(grid, f0, SENDER, STREAM, MumbleProto::VideoState_Codec_VP8);

	const QImage afterKeyframe = grid.surfaceFor(SENDER, STREAM);
	QVERIFY(!afterKeyframe.isNull());

	// Frame 1 never arrives; frame 2 does. Feeding it to the decoder would "succeed" with garbage, so
	// the grid must not: the canvas stays exactly the frame-0 picture and one keyframe request goes out.
	deliverFrame(grid, f2, SENDER, STREAM, MumbleProto::VideoState_Codec_VP8);

	QCOMPARE(meanDifference(grid.surfaceFor(SENDER, STREAM), afterKeyframe), 0.0);
	QCOMPARE(needed.count(), 1);

	// Further inter-frames while frozen change nothing and do not spam requests.
	const auto f3 = encoder.encode(source.render(3), STREAM, 3, 3, false);
	deliverFrame(grid, f3, SENDER, STREAM, MumbleProto::VideoState_Codec_VP8);

	QCOMPARE(meanDifference(grid.surfaceFor(SENDER, STREAM), afterKeyframe), 0.0);
	QCOMPARE(needed.count(), 1);
}

void TestVideoGrid::decodeResumesAtTheNextKeyframe() {
	VP8Encoder encoder;
	encoder.setBitrate(600);
	encoder.setFramerate(30);

	SyntheticVideoSource source(128, 128);
	source.setChangeRatio(100);

	VideoGrid grid;
	QSignalSpy needed(&grid, &VideoGrid::keyframeNeeded);

	announce(grid, SENDER, STREAM, MumbleProto::VideoState_Codec_VP8);

	const auto f0 = encoder.encode(source.render(0), STREAM, 0, 0, true);
	encoder.encode(source.render(1), STREAM, 1, 1, false); // lost on the wire
	const auto f2 = encoder.encode(source.render(2), STREAM, 2, 2, false);
	const auto f3 = encoder.encode(source.render(3), STREAM, 3, 3, true); // the answer to the request

	deliverFrame(grid, f0, SENDER, STREAM, MumbleProto::VideoState_Codec_VP8);
	deliverFrame(grid, f2, SENDER, STREAM, MumbleProto::VideoState_Codec_VP8);

	const QImage frozen = grid.surfaceFor(SENDER, STREAM);
	QCOMPARE(needed.count(), 1);

	// The keyframe unfreezes the stream: the canvas moves off the frozen picture, and no further
	// request is emitted.
	deliverFrame(grid, f3, SENDER, STREAM, MumbleProto::VideoState_Codec_VP8);

	QVERIFY(meanDifference(grid.surfaceFor(SENDER, STREAM), frozen) > 1.0);
	QCOMPARE(needed.count(), 1);
}

// The reassembler delivers units in completion order, not frame order: a fragment of frame N can finish
// reassembling long after frame N+1 played. Decoding the latecomer would rewind the decoder's reference
// state and corrupt everything after it, so stale frames are dropped before the decoder sees them.
void TestVideoGrid::aStaleVp8FrameArrivingLateIsDropped() {
	VP8Encoder encoder;
	encoder.setBitrate(600);
	encoder.setFramerate(30);

	SyntheticVideoSource source(128, 128);
	source.setChangeRatio(100);

	VideoGrid grid;
	QSignalSpy needed(&grid, &VideoGrid::keyframeNeeded);

	announce(grid, SENDER, STREAM, MumbleProto::VideoState_Codec_VP8);

	const auto f0 = encoder.encode(source.render(0), STREAM, 0, 0, true);
	const auto f1 = encoder.encode(source.render(1), STREAM, 1, 1, false);
	const auto f2 = encoder.encode(source.render(2), STREAM, 2, 2, false);

	deliverFrame(grid, f0, SENDER, STREAM, MumbleProto::VideoState_Codec_VP8);
	deliverFrame(grid, f1, SENDER, STREAM, MumbleProto::VideoState_Codec_VP8);
	deliverFrame(grid, f2, SENDER, STREAM, MumbleProto::VideoState_Codec_VP8);

	const QImage current = grid.surfaceFor(SENDER, STREAM);

	// Frame 1 shows up again, late. The canvas must not move and the decoder must not be poisoned:
	// no freeze, no keyframe request, nothing.
	deliverFrame(grid, f1, SENDER, STREAM, MumbleProto::VideoState_Codec_VP8);

	QCOMPARE(meanDifference(grid.surfaceFor(SENDER, STREAM), current), 0.0);
	QCOMPARE(needed.count(), 0);
}

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

	// A second stream from the same sender - the name belongs to the person, not to any one stream.
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
		grid.onVideoUnitReceived(SENDER, STREAM, 0, true, 0, 0, garbage);
	}

	QCOMPARE(needed.count(), 0);

	grid.onVideoUnitReceived(SENDER, STREAM, 0, true, 0, 0, garbage);
	QCOMPARE(needed.count(), 1);
	QCOMPARE(needed.at(0).at(0).toUInt(), SENDER);
	QCOMPARE(needed.at(0).at(1).toUInt(), STREAM);

	// The counter restarts after each report, so a sender that never answers is asked again only after
	// another full run - not on every subsequent unit.
	for (int i = 0; i < VideoGrid::KEYFRAME_REQUEST_AFTER_FAILURES - 1; ++i) {
		grid.onVideoUnitReceived(SENDER, STREAM, 0, true, 0, 0, garbage);
	}

	QCOMPARE(needed.count(), 1);

	// A stream announced with a codec this build cannot decode must never generate requests: they would
	// ask for something no keyframe can fix.
	VideoGrid unknowing;
	QSignalSpy futile(&unknowing, &VideoGrid::keyframeNeeded);

	announce(unknowing, SENDER, STREAM, MumbleProto::VideoState_Codec_CODEC_UNKNOWN);

	for (int i = 0; i < 3 * VideoGrid::KEYFRAME_REQUEST_AFTER_FAILURES; ++i) {
		unknowing.onVideoUnitReceived(SENDER, STREAM, 0, true, 0, 0, garbage);
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
	grid.onVideoUnitReceived(SENDER, STREAM, 0, true, 100000, 100000, encoded);

	QCOMPARE(grid.senderCount(), 0);
	QVERIFY(grid.surfaceFor(SENDER, STREAM).isNull());

	// Exactly at the boundary is still refused, since the tile would extend past it.
	grid.onVideoUnitReceived(SENDER, STREAM, 0, true, static_cast< unsigned int >(VideoGrid::MAX_SURFACE_WIDTH), 0,
							 encoded);
	QCOMPARE(grid.senderCount(), 0);
}

void TestVideoGrid::nonJpegDataIsRefused() {
	VideoGrid grid;

	announce(grid, SENDER, STREAM);
	grid.onVideoUnitReceived(SENDER, STREAM, 0, true, 0, 0, QByteArray("not an image at all"));
	QCOMPARE(grid.senderCount(), 0);

	grid.onVideoUnitReceived(SENDER, STREAM, 0, true, 0, 0, QByteArray());
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

	grid.onVideoUnitReceived(SENDER, STREAM, 0, true, 0, 0, encoded);
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

// A sender that stops and restarts a camera announces a fresh stream id. If the end of the old stream was
// lost on the way - a dropped control message - the viewer would otherwise keep the old surface as a
// stuck last frame beside the new picture. The newer announcement of the same kind wins.
void TestVideoGrid::aRestartedStreamOfTheSameKindReplacesTheOld() {
	VideoGrid grid;

	QImage frame(64, 64, QImage::Format_RGB32);
	frame.fill(Qt::red);

	QByteArray encoded;
	QBuffer buffer(&encoded);
	buffer.open(QIODevice::WriteOnly);
	QVERIFY(frame.save(&buffer, "JPEG", 90));
	buffer.close();

	announce(grid, SENDER, 1, MumbleProto::VideoState_Codec_TiledImage, MumbleProto::VideoState_SourceKind_Camera);
	grid.onVideoUnitReceived(SENDER, 1, 0, true, 0, 0, encoded);
	QCOMPARE(grid.surfaceFor(SENDER, 1).size(), QSize(64, 64));

	// No removeSender(SENDER, 1) in between: the end-of-stream never arrived.
	announce(grid, SENDER, 2, MumbleProto::VideoState_Codec_TiledImage, MumbleProto::VideoState_SourceKind_Camera);

	QVERIFY2(grid.surfaceFor(SENDER, 1).isNull(), "the stale camera surface survived a camera restart");
	QCOMPARE(grid.senderCount(), 0);

	grid.onVideoUnitReceived(SENDER, 2, 0, true, 0, 0, encoded);
	QCOMPARE(grid.surfaceFor(SENDER, 2).size(), QSize(64, 64));
	QCOMPARE(grid.senderCount(), 1);
}
