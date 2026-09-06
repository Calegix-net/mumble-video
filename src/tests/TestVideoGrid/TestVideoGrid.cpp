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

#include <algorithm>
#include <cstdint>
#include <vector>

namespace {

constexpr unsigned int SENDER = 5;
constexpr unsigned int STREAM = 1;

/// Announces a stream, as the VideoState message does, and marks it watched. Nothing is decoded without
/// the announcement: the payload never says which codec produced it, so an unannounced stream has no
/// decoder to hand it to. A newly announced stream also starts out an unwatched preview - see
/// Surface::watching - so this additionally opts it in, matching what every test in this file other than
/// the ones about the preview state itself actually wants: units delivered right after this decode.
void announce(VideoGrid &grid, unsigned int sender, unsigned int stream,
			  int codec      = MumbleProto::VideoState_Codec_TiledImage,
			  int sourceKind = MumbleProto::VideoState_SourceKind_SOURCE_UNKNOWN) {
	grid.setStreamCodec(sender, stream, sourceKind, codec);
	grid.setWatching(sender, stream, true);
}

/// TiledImage tiles now decode on a background thread - see VideoGrid::onTiledImageTileDecoded() - so a
/// test that just called onVideoUnitReceived() for one has to pump the event loop before checking the
/// result, or it is checking a decode that has not actually happened yet. VP8 stays fully synchronous and
/// never needs this. 200ms is generous for a JPEG this small on an otherwise-idle thread pool - qWait()
/// does process events for the full duration given, not just until something happens, so this is a real
/// per-call cost, not a ceiling - but reliability against a slower or loaded machine matters more here
/// than shaving this test suite's total run time.
void waitForAsyncDecode() {
	QTest::qWait(200);
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

	if (codec == MumbleProto::VideoState_Codec_TiledImage) {
		waitForAsyncDecode();
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
	void backgroundTilesPaintInArrivalOrder();
	void lateNetworkTilesCannotOverwriteNewPixels();
	void resizingPreservesWatchingAndDiscardsOldPixels();
	void unwatchDiscardsPendingTiles();
	void announcementMakesTheDockVisible();
	void unsubscribeRepliesPreserveTheTileAndSubsequentWatch();
	void removingRemoteVideoKeepsLocalPreviewVisible();
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
	void aNewStreamStartsAsAnUnwatchedPreview();
	void nonJpegDataIsRefused();
	void itPaintsWithoutCrashing();
	void rapidWatchToggleWhileFramesArriveDoesNotCrash();
	void firstFrameShowingTheDockDoesNotReenterUnsafely();
	void watchedStreamGoingSilentIsDroppedAsStale();
	void watchedStreamStillActiveIsNotDroppedAsStale();
	void routineTileUpdatesDoNotReflowEveryTilesControls();
	void tiledImageDecodeDoesNotBlockTheCallingThread();
	void stalledStreamGetsOneKeyframeRequest();
	void senderSessionsReportsEachSessionOnceWatchedOrNot();
	void hoverEventsDoNotTouchWidgetsInsideQtsOwnDispatch();
	void resizingDoesNotChurnTheHoverBarsVisibility();
};

namespace {
class ControlledDecodeGrid : public VideoGrid {
public:
	std::shared_ptr< PendingTile > queueTile(const QColor &color) {
		auto tile   = std::make_shared< PendingTile >();
		tile->image = QImage(32, 32, QImage::Format_RGB32);
		tile->image.fill(color);
		m_surfaces.at(surfaceKey(SENDER, STREAM)).pendingTiles.push_back(tile);
		return tile;
	}

	void finish(const std::shared_ptr< PendingTile > &tile) {
		tile->ready = true;
		applyReadyTiles(SENDER, STREAM);
	}
};
} // namespace

void TestVideoGrid::backgroundTilesPaintInArrivalOrder() {
	ControlledDecodeGrid grid;
	announce(grid, SENDER, STREAM);
	const auto older = grid.queueTile(Qt::red);
	const auto newer = grid.queueTile(Qt::green);
	grid.finish(newer);
	QVERIFY(grid.surfaceFor(SENDER, STREAM).isNull());
	grid.finish(older);
	QCOMPARE(grid.surfaceFor(SENDER, STREAM).pixelColor(0, 0), QColor(Qt::green));
}

void TestVideoGrid::unwatchDiscardsPendingTiles() {
	ControlledDecodeGrid grid;
	announce(grid, SENDER, STREAM);
	const auto old = grid.queueTile(Qt::red);
	grid.setWatching(SENDER, STREAM, false);
	grid.setWatching(SENDER, STREAM, true);
	grid.finish(old);
	QVERIFY(grid.surfaceFor(SENDER, STREAM).isNull());
	const auto current = grid.queueTile(Qt::green);
	grid.finish(current);
	QCOMPARE(grid.surfaceFor(SENDER, STREAM).pixelColor(0, 0), QColor(Qt::green));
}

void TestVideoGrid::announcementMakesTheDockVisible() {
	VideoGrid grid;
	grid.hide();
	connect(&grid, &VideoGrid::senderCountChanged, &grid, [&](int count) { grid.setVisible(count > 0); });
	grid.setStreamCodec(SENDER, STREAM, MumbleProto::VideoState_SourceKind_Camera, MumbleProto::VideoState_Codec_VP8);
	QVERIFY(grid.isVisible());
	QCOMPARE(grid.senderCount(), 1);
}

void TestVideoGrid::unsubscribeRepliesPreserveTheTileAndSubsequentWatch() {
	VideoGrid grid;
	announce(grid, SENDER, STREAM);
	grid.setWatching(SENDER, STREAM, false);
	QVERIFY(grid.consumeUnsubscribeAcknowledgement(SENDER, STREAM));
	QCOMPARE(grid.senderCount(), 1);
	QVERIFY(!grid.consumeUnsubscribeAcknowledgement(SENDER, STREAM));

	grid.setWatching(SENDER, STREAM, true);
	grid.setWatching(SENDER, STREAM, false);
	grid.setWatching(SENDER, STREAM, true);
	QVERIFY(grid.consumeUnsubscribeAcknowledgement(SENDER, STREAM));
	QSignalSpy toggles(&grid, &VideoGrid::watchToggled);
	grid.setWatching(SENDER, STREAM, true);
	QCOMPARE(toggles.count(), 0);
	// An unsolicited withdrawal must still reach the caller's permission-revocation handling.
	QVERIFY(!grid.consumeUnsubscribeAcknowledgement(SENDER, STREAM));
}

void TestVideoGrid::removingRemoteVideoKeepsLocalPreviewVisible() {
	VideoGrid grid;
	connect(&grid, &VideoGrid::senderCountChanged, &grid, [&](int count) { grid.setVisible(count > 0); });
	QImage preview(32, 32, QImage::Format_RGB32);
	preview.fill(Qt::red);
	grid.setSelfCameraFrame(preview);
	grid.setStreamCodec(SENDER, STREAM, MumbleProto::VideoState_SourceKind_Camera, MumbleProto::VideoState_Codec_VP8);
	grid.removeSender(SENDER, STREAM);
	QVERIFY(grid.isVisible());
}

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
	waitForAsyncDecode();
	QCOMPARE(grid.surfaceFor(SENDER, STREAM).size(), QSize(64, 64));

	// A tile further right and down forces the surface to grow.
	grid.onVideoUnitReceived(SENDER, STREAM, 0, true, 64, 64, encoded);
	waitForAsyncDecode();
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
	waitForAsyncDecode();
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
	waitForAsyncDecode();
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
	waitForAsyncDecode();
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
	// Announcements expose the watch controls before any media can arrive.
	QCOMPARE(spy.count(), 2);
	spy.clear();
	grid.onVideoUnitReceived(1, STREAM, 0, true, 0, 0, encoded);
	grid.onVideoUnitReceived(2, STREAM, 0, true, 0, 0, encoded);
	waitForAsyncDecode();
	QCOMPARE(grid.senderCount(), 2);
	QCOMPARE(spy.count(), 2);

	// Further tiles from a known sender are not a new arrival.
	grid.onVideoUnitReceived(1, STREAM, 0, true, 0, 0, encoded);
	waitForAsyncDecode();
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

	waitForAsyncDecode();
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
	waitForAsyncDecode();
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

	// The raw announcement, not the announce() helper: this test wants the real just-announced state,
	// still an unwatched preview, not the helper's "and mark it watched" convenience for every other test
	// in this file.
	grid.setStreamCodec(SENDER, STREAM, MumbleProto::VideoState_SourceKind_SOURCE_UNKNOWN,
						MumbleProto::VideoState_Codec_TiledImage);

	// Unwatched from the moment it is announced, so it already occupies a cell as a preview placeholder -
	// it does not wait on a canvas that, unwatched, will never fill.
	QCOMPARE(grid.senderCount(), 1);

	QImage target(320, 240, QImage::Format_RGB32);
	grid.render(&target);

	// Watching it before any tile has actually arrived is the other blank case worth covering: genuinely
	// nothing decoded yet, not a placeholder by choice. This must not divide by a zero-tile layout either.
	grid.setWatching(SENDER, STREAM, true);
	QCOMPARE(grid.senderCount(), 0);

	grid.render(&target);
}

// The eyeball-preview feature itself: a stream nobody has opted into watching yet must not be decoded -
// the whole point is to not pay for a picture nobody asked to see - but must still show something, or a
// screen full of people sharing would look like nothing was happening at all.
void TestVideoGrid::aNewStreamStartsAsAnUnwatchedPreview() {
	VideoGrid grid;

	QImage tile(32, 32, QImage::Format_RGB32);
	tile.fill(Qt::green);

	QByteArray encoded;
	QBuffer buffer(&encoded);
	buffer.open(QIODevice::WriteOnly);
	QVERIFY(tile.save(&buffer, "JPEG", 90));
	buffer.close();

	// Announced directly, not through the announce() helper, since the helper's whole job is to opt every
	// other test in this file out of exactly the state this one exists to check.
	grid.setStreamCodec(SENDER, STREAM, MumbleProto::VideoState_SourceKind_SOURCE_UNKNOWN,
						MumbleProto::VideoState_Codec_TiledImage);

	// Claims its cell immediately, as a preview - but nothing is decoded into it.
	QCOMPARE(grid.senderCount(), 1);
	QVERIFY(grid.surfaceFor(SENDER, STREAM).isNull());

	// A tile arriving before anyone opted in is dropped, not queued - see onVideoUnitReceived().
	grid.onVideoUnitReceived(SENDER, STREAM, 0, true, 0, 0, encoded);
	QVERIFY(grid.surfaceFor(SENDER, STREAM).isNull());

	// Clicking the eyeball (setWatching) is what actually starts decoding.
	grid.setWatching(SENDER, STREAM, true);
	grid.onVideoUnitReceived(SENDER, STREAM, 0, true, 0, 0, encoded);
	waitForAsyncDecode();
	QCOMPARE(grid.surfaceFor(SENDER, STREAM).size(), QSize(32, 32));

	// Un-watching again leaves the last picture in place rather than clearing it - paintEvent() falls back
	// to the placeholder instead of painting it, but it is still there if watching resumes.
	grid.setWatching(SENDER, STREAM, false);
	QVERIFY(!grid.surfaceFor(SENDER, STREAM).isNull());
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
	// make the client allocate a surface to match. growToFit()'s bounds check now runs inside the
	// asynchronous continuation (see onTiledImageTileDecoded()), not inline here - waitForAsyncDecode()
	// is what makes this test actually exercise that check, rather than merely observing that nothing has
	// happened yet because the decode has not run at all.
	announce(grid, SENDER, STREAM);
	grid.onVideoUnitReceived(SENDER, STREAM, 0, true, 100000, 100000, encoded);
	waitForAsyncDecode();

	QCOMPARE(grid.senderCount(), 0);
	QVERIFY(grid.surfaceFor(SENDER, STREAM).isNull());

	// Exactly at the boundary is still refused, since the tile would extend past it.
	grid.onVideoUnitReceived(SENDER, STREAM, 0, true, static_cast< unsigned int >(VideoGrid::MAX_SURFACE_WIDTH), 0,
							 encoded);
	waitForAsyncDecode();
	QCOMPARE(grid.senderCount(), 0);
}

void TestVideoGrid::nonJpegDataIsRefused() {
	VideoGrid grid;

	// waitForAsyncDecode() after each call is what makes this test actually exercise the decode-failure
	// path asynchronously, rather than merely observing that the (now backgrounded) decode has not
	// finished yet.
	announce(grid, SENDER, STREAM);
	grid.onVideoUnitReceived(SENDER, STREAM, 0, true, 0, 0, QByteArray("not an image at all"));
	waitForAsyncDecode();
	QCOMPARE(grid.senderCount(), 0);

	grid.onVideoUnitReceived(SENDER, STREAM, 0, true, 0, 0, QByteArray());
	waitForAsyncDecode();
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
	waitForAsyncDecode();
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

// Regression test for a crash confirmed under real interactive use (not just an idle launch, which is all
// any earlier testing in this area had ever done): clicking a tile's watch button while frames were
// actively arriving crashed the client. Root cause was relayout() being reentered synchronously from
// several different directions - see VideoGrid.h's m_relayoutInProgress - while relayoutControls() was
// still creating or destroying real QWidgets. This drives the same kind of interleaving directly: toggling
// watch, delivering frames, and resizing the widget, all tightly interleaved rather than one at a time, on
// several senders at once. Reaching the end at all is most of what this test is for - a genuine reentrancy
// bug here crashes the whole test binary rather than failing a single QCOMPARE.
void TestVideoGrid::rapidWatchToggleWhileFramesArriveDoesNotCrash() {
	SyntheticVideoSource source(64, 64);
	TiledImageEncoder encoder;

	VideoGrid grid;
	grid.resize(800, 600);

	constexpr unsigned int SENDERS = 6;

	for (unsigned int sender = 1; sender <= SENDERS; ++sender) {
		grid.setStreamCodec(sender, STREAM, MumbleProto::VideoState_SourceKind_SOURCE_UNKNOWN,
							MumbleProto::VideoState_Codec_TiledImage);
	}

	// Every stream starts as an unwatched preview - see Surface::watching - which is exactly the state a
	// real call is in the instant everyone has joined: several placeholders, nothing decoding yet, all
	// waiting on someone to click their eyeball.
	QCOMPARE(grid.senderCount(), static_cast< int >(SENDERS));

	for (int round = 0; round < 60; ++round) {
		const unsigned int sender = (static_cast< unsigned int >(round) % SENDERS) + 1;

		// The click: toggling watch is exactly what the real watch button's clicked() handler does -
		// setWatching() is the one place that logic lives, whether it is reached by a click or, as here,
		// directly.
		grid.setWatching(sender, STREAM, true);

		// The frame: arriving in the same instant a real one plausibly could, right as watching flips on -
		// this is what flips a surface's canvas from blank to something real, and is what emits
		// senderCountChanged() the first time.
		auto units = encoder.encode(source.render(sender), STREAM, 1, 1);

		for (const EncodedVideoUnit &unit : units) {
			grid.onVideoUnitReceived(sender, STREAM, unit.header.frameNumber, unit.header.isKeyframe, unit.header.x,
									 unit.header.y,
									 QByteArray(reinterpret_cast< const char * >(unit.payload.data()),
												static_cast< int >(unit.payload.size())));
		}

		encoder.reset();

		// The resize: what a video dock becoming visible for the first time, a splitter drag, or a plain
		// window resize does mid-call - each one synchronously re-enters relayout() by way of
		// resizeEvent().
		grid.resize(700 + (round % 5) * 40, 500 + (round % 3) * 30);

		// And toggling back off - the other half of "somebody keeps clicking the eyeball on and off".
		grid.setWatching(sender, STREAM, false);
	}

	QImage target(800, 600, QImage::Format_RGB32);
	grid.render(&target);
}

// Forces the specific reentrancy m_relayoutInProgress guards against - a resize, triggered from inside a
// senderCountChanged connection, synchronously reentering resizeEvent() -> relayout() before
// onVideoUnitReceived() has returned to run its own trailing relayout() call - directly and
// deterministically, standing in for what a real MainWindow's video dock does the moment it is shown for
// the first time. Written to actually prove the guard matters, not just assert it exists: tried against a
// build with m_relayoutInProgress temporarily disabled, twice - once through a real QDockWidget, then in
// this more direct form after the offscreen QPA platform this suite runs under turned out not to deliver a
// real synchronous resize through QDockWidget::setVisible() the way an on-screen window does. Neither
// version crashed or failed without the guard, even though the reentrant relayoutControls() call
// unmistakably ran (confirmable by a breakpoint, not by anything this test asserts). Left in specifically
// because of that, not despite it: it is honest coverage of the mechanism the guard targets, not proof the
// guard is what the reported crash needed - see the commit message for what is and is not established.
void TestVideoGrid::firstFrameShowingTheDockDoesNotReenterUnsafely() {
	VideoGrid grid;
	grid.resize(400, 300);

	bool sawReentrantResize = false;

	QObject::connect(&grid, &VideoGrid::senderCountChanged, &grid, [&](int count) {
		if (count > 0 && !sawReentrantResize) {
			sawReentrantResize = true;

			// Standing in for MainWindow's video dock becoming visible for the first time - a resize this
			// deep inside onVideoUnitReceived() is exactly the reentrant resizeEvent() -> relayout() call
			// the mechanism above describes, happening for real rather than being merely plausible.
			grid.resize(800, 600);
		}
	});

	SyntheticVideoSource source(64, 64);
	TiledImageEncoder encoder;

	grid.setStreamCodec(SENDER, STREAM, MumbleProto::VideoState_SourceKind_SOURCE_UNKNOWN,
						MumbleProto::VideoState_Codec_TiledImage);
	grid.setWatching(SENDER, STREAM, true);

	// The first frame after watching starts is exactly what flips a surface's canvas from blank to
	// something real inside onVideoUnitReceived() - which is what emits senderCountChanged() - which, by
	// way of the handler above, resizes this same widget synchronously, from inside this same call.
	auto units = encoder.encode(source.render(1), STREAM, 1, 1);

	for (const EncodedVideoUnit &unit : units) {
		grid.onVideoUnitReceived(
			SENDER, STREAM, unit.header.frameNumber, unit.header.isKeyframe, unit.header.x, unit.header.y,
			QByteArray(reinterpret_cast< const char * >(unit.payload.data()), static_cast< int >(unit.payload.size())));
	}

	waitForAsyncDecode();

	QVERIFY(sawReentrantResize);
	QVERIFY(!grid.surfaceFor(SENDER, STREAM).isNull());
}

// Regression test for a real report: when someone's client crashed mid-share, their frozen last frame
// stayed on every remaining viewer's screen indefinitely - not merely for a few seconds, but never
// clearing on its own. msgUserRemove already exists to clean up a departed sender's video (added before
// this session, confirmed present and correct), but it depends entirely on the server actually noticing
// the disconnect and relaying that notice - and a crash that leaves the process in a non-terminating
// state (still writing a crash dump, say) can leave that noticing undone indefinitely, with nothing
// downstream able to help. This exercises the watchdog that exists for exactly that case: judging
// staleness from silence on the stream itself, needing no notification at all.
void TestVideoGrid::watchedStreamGoingSilentIsDroppedAsStale() {
	VideoGrid grid;

	// Comfortably above waitForAsyncDecode()'s own 200ms: the staleness clock starts ticking the instant
	// onVideoUnitReceived() is called, not once its (now asynchronous) decode actually finishes, so that
	// wait already eats into this budget before the "still there" check below even runs.
	grid.setStaleStreamTimeoutMsecForTesting(350);

	QSignalSpy staleSpy(&grid, &VideoGrid::streamWentStale);

	grid.setStreamCodec(SENDER, STREAM, MumbleProto::VideoState_SourceKind_SOURCE_UNKNOWN,
						MumbleProto::VideoState_Codec_TiledImage);
	grid.setWatching(SENDER, STREAM, true);

	QImage tile(32, 32, QImage::Format_RGB32);
	tile.fill(Qt::green);

	QByteArray encoded;
	QBuffer buffer(&encoded);
	buffer.open(QIODevice::WriteOnly);
	QVERIFY(tile.save(&buffer, "JPEG", 90));
	buffer.close();

	grid.onVideoUnitReceived(SENDER, STREAM, 0, true, 0, 0, encoded);
	waitForAsyncDecode();
	QVERIFY(!grid.surfaceFor(SENDER, STREAM).isNull());

	// Real silence, past the (shrunk, for this test) timeout - the actual timer firing is what drops it,
	// not a direct call to the check it drives.
	QTest::qWait(500);

	QCOMPARE(staleSpy.count(), 1);
	QVERIFY(grid.surfaceFor(SENDER, STREAM).isNull());
}

// The other half of the same property: a stream that keeps producing units, even slowly, must never be
// mistaken for a dead one just because some real wall-clock time has passed in total.
void TestVideoGrid::watchedStreamStillActiveIsNotDroppedAsStale() {
	VideoGrid grid;
	grid.setStaleStreamTimeoutMsecForTesting(300);

	QSignalSpy staleSpy(&grid, &VideoGrid::streamWentStale);

	grid.setStreamCodec(SENDER, STREAM, MumbleProto::VideoState_SourceKind_SOURCE_UNKNOWN,
						MumbleProto::VideoState_Codec_TiledImage);
	grid.setWatching(SENDER, STREAM, true);

	QImage tile(32, 32, QImage::Format_RGB32);
	tile.fill(Qt::green);

	QByteArray encoded;
	QBuffer buffer(&encoded);
	buffer.open(QIODevice::WriteOnly);
	QVERIFY(tile.save(&buffer, "JPEG", 90));
	buffer.close();

	// Each gap alone stays comfortably under the timeout, even though the total elapsed time by the end
	// exceeds it several times over - what matters is the gap since the last unit, not the running total.
	for (int i = 0; i < 5; ++i) {
		grid.onVideoUnitReceived(SENDER, STREAM, static_cast< quint64 >(i), true, 0, 0, encoded);
		QTest::qWait(100);
	}

	QCOMPARE(staleSpy.count(), 0);
	QVERIFY(!grid.surfaceFor(SENDER, STREAM).isNull());
}

// Regression test for a real report: viewers watching an active screen share or camera - never the person
// sharing, whose own preview never goes through this path at all - saw real, sustained lag.
// onVideoUnitReceived() used to call the full relayout() on every single incoming unit, and
// relayoutControls() - the expensive part of that - does real per-tile widget work for every tile in the
// grid, not just the one that changed. This proves a routine tile update, within a surface that already
// has a picture, no longer pays that cost.
void TestVideoGrid::routineTileUpdatesDoNotReflowEveryTilesControls() {
	SyntheticVideoSource source(640, 480);
	source.setChangeRatio(100);

	TiledImageEncoder encoderA;
	TiledImageEncoder encoderB;

	VideoGrid grid;
	grid.resize(800, 600);

	// A second, unrelated sender in the grid too - relayoutControls(), if it ran, would touch this one's
	// control bar as well, exactly the wasted work being tested against.
	grid.setStreamCodec(2, STREAM, MumbleProto::VideoState_SourceKind_SOURCE_UNKNOWN,
						MumbleProto::VideoState_Codec_TiledImage);
	grid.setWatching(2, STREAM, true);

	grid.setStreamCodec(SENDER, STREAM, MumbleProto::VideoState_SourceKind_SOURCE_UNKNOWN,
						MumbleProto::VideoState_Codec_TiledImage);
	grid.setWatching(SENDER, STREAM, true);

	const auto deliver = [&](unsigned int sender, TiledImageEncoder &encoder, quint64 frameNumber) {
		const std::vector< EncodedVideoUnit > units =
			encoder.encode(source.render(frameNumber), STREAM, frameNumber, frameNumber);

		for (const EncodedVideoUnit &unit : units) {
			grid.onVideoUnitReceived(sender, STREAM, unit.header.frameNumber, unit.header.isKeyframe, unit.header.x,
									 unit.header.y,
									 QByteArray(reinterpret_cast< const char * >(unit.payload.data()),
												static_cast< int >(unit.payload.size())));
		}

		waitForAsyncDecode();
	};

	// The first frame for each surface is the wasBlank transition - the one case that legitimately still
	// needs the full relayout(), since it is what makes the tile occupy a cell at all.
	deliver(2, encoderB, 0);
	deliver(SENDER, encoderA, 0);

	const int countAfterFirstFrames = grid.relayoutControlsCallCountForTesting();
	QVERIFY(countAfterFirstFrames > 0);

	// Many more frames of ordinary content change, on an already-populated surface - the case this fix is
	// actually about.
	for (quint64 i = 1; i <= 20; ++i) {
		deliver(SENDER, encoderA, i);
	}

	QCOMPARE(grid.relayoutControlsCallCountForTesting(), countAfterFirstFrames);
}

// Regression test for the same lag report the previous test covers, from the other end of the fix:
// JPEG decode for TiledImage units - the busiest source of units this class ever handles - now runs on a
// background thread rather than inline, so the real, measurable decode work no longer competes with the
// GUI thread's own paint and input handling. Proven independently via a temporary qWarning() printing
// QThread::currentThread() from inside decodeJpegTile() during manual verification - every decode ran on
// a distinct worker-pool thread, never once on the app's own thread - but that instrumentation does not
// belong in shipped code, so this test instead asserts the same fact by its one directly observable
// consequence: onVideoUnitReceived() must return before the decode it just dispatched has necessarily
// finished, which a synchronous implementation could never do.
void TestVideoGrid::tiledImageDecodeDoesNotBlockTheCallingThread() {
	SyntheticVideoSource source(64, 64);
	TiledImageEncoder encoder;

	VideoGrid grid;

	grid.setStreamCodec(SENDER, STREAM, MumbleProto::VideoState_SourceKind_SOURCE_UNKNOWN,
						MumbleProto::VideoState_Codec_TiledImage);
	grid.setWatching(SENDER, STREAM, true);

	const std::vector< EncodedVideoUnit > units = encoder.encode(source.render(0), STREAM, 1, 1);
	QVERIFY(!units.empty());

	for (const EncodedVideoUnit &unit : units) {
		grid.onVideoUnitReceived(
			SENDER, STREAM, unit.header.frameNumber, unit.header.isKeyframe, unit.header.x, unit.header.y,
			QByteArray(reinterpret_cast< const char * >(unit.payload.data()), static_cast< int >(unit.payload.size())));
	}

	// Every one of those calls has already returned, but nothing has actually been painted into the
	// surface yet: the decode they dispatched has not necessarily run, because it was handed to a
	// background thread rather than done inline. QFutureWatcher::finished() cannot fire until this
	// thread's own event loop actually turns - not yet, on the very next line after dispatching it - so a
	// still-null surface here is not a timing fluke, it is the one thing a synchronous implementation
	// could never produce.
	QVERIFY(grid.surfaceFor(SENDER, STREAM).isNull());

	waitForAsyncDecode();

	QVERIFY(!grid.surfaceFor(SENDER, STREAM).isNull());
}

// TiledImage has no proactive recovery signal of its own the way VP8's frame-continuity tracking does -
// before this, a watched stream that stalled anywhere short of the full stale-drop window (see
// watchedStreamGoingSilentIsDroppedAsStale) just sat there until the periodic refresh cycle eventually
// got around to it, which for a large grid can be the whole multi-second cycle. This proves a stall past
// the (shrunk, for this test) stall threshold gets exactly one keyframeNeeded() request - not zero, and
// not a flood of them for the same silence - while staying well short of the (deliberately lengthened,
// here) drop threshold, so the surface is still there afterward.
void TestVideoGrid::stalledStreamGetsOneKeyframeRequest() {
	// 350ms, not the tighter 100ms first tried here: waitForAsyncDecode() below already burns 200ms on
	// every TiledImage delivery (see its own comment), and that clock starts at onVideoUnitReceived(), not
	// at decode completion - so a stall threshold shorter than the decode wait would already have tripped
	// by the time the first assertion below even runs. Same reasoning, same margin, as the sibling stale-
	// drop test (watchedStreamGoingSilentIsDroppedAsStale).
	VideoGrid grid;
	grid.setStallRefreshTimeoutMsecForTesting(350);
	grid.setStaleStreamTimeoutMsecForTesting(8000);

	QSignalSpy needed(&grid, &VideoGrid::keyframeNeeded);

	grid.setStreamCodec(SENDER, STREAM, MumbleProto::VideoState_SourceKind_SOURCE_UNKNOWN,
						MumbleProto::VideoState_Codec_TiledImage);
	grid.setWatching(SENDER, STREAM, true);

	QImage tile(32, 32, QImage::Format_RGB32);
	tile.fill(Qt::green);

	QByteArray encoded;
	QBuffer buffer(&encoded);
	buffer.open(QIODevice::WriteOnly);
	QVERIFY(tile.save(&buffer, "JPEG", 90));
	buffer.close();

	grid.onVideoUnitReceived(SENDER, STREAM, 0, true, 0, 0, encoded);
	waitForAsyncDecode();

	QCOMPARE(needed.count(), 0);

	// Real silence, past the (shrunk) stall threshold but nowhere near the (deliberately lengthened, for
	// this test) stale-drop one.
	QTest::qWait(400);

	QCOMPARE(needed.count(), 1);
	QCOMPARE(needed.at(0).at(0).toUInt(), SENDER);
	QCOMPARE(needed.at(0).at(1).toUInt(), STREAM);

	// Only the drop threshold removes the surface, and this test kept that far away - the stall request
	// is a nudge, not a removal.
	QVERIFY(!grid.surfaceFor(SENDER, STREAM).isNull());

	// Continued silence for the same stall must not spam further requests.
	QTest::qWait(400);
	QCOMPARE(needed.count(), 1);

	// A fresh unit resets the stall clock, so the next silence earns its own request.
	grid.onVideoUnitReceived(SENDER, STREAM, 1, true, 0, 0, encoded);
	waitForAsyncDecode();
	QCOMPARE(needed.count(), 1);

	QTest::qWait(400);
	QCOMPARE(needed.count(), 2);
}

void TestVideoGrid::lateNetworkTilesCannotOverwriteNewPixels() {
	VideoGrid grid;
	announce(grid, SENDER, STREAM, MumbleProto::VideoState_Codec_TiledImage);
	QImage blue(32, 32, QImage::Format_RGB32);
	blue.fill(Qt::blue);
	QImage red(32, 32, QImage::Format_RGB32);
	red.fill(Qt::red);
	auto jpeg = [](const QImage &image) {
		QByteArray bytes;
		QBuffer buffer(&bytes);
		buffer.open(QIODevice::WriteOnly);
		image.save(&buffer, "JPEG");
		return bytes;
	};
	grid.onVideoUnitReceived(SENDER, STREAM, 20, true, 0, 0, jpeg(blue));
	grid.onVideoUnitReceived(SENDER, STREAM, 19, true, 0, 0, jpeg(red));
	waitForAsyncDecode();
	QVERIFY(grid.surfaceFor(SENDER, STREAM).pixelColor(0, 0).blue() > 200);
}

void TestVideoGrid::resizingPreservesWatchingAndDiscardsOldPixels() {
	VideoGrid grid;
	announce(grid, SENDER, 10, MumbleProto::VideoState_Codec_TiledImage, MumbleProto::VideoState_SourceKind_Window);
	QSignalSpy subscriptions(&grid, &VideoGrid::watchToggled);
	grid.setStreamCodec(SENDER, 11, MumbleProto::VideoState_SourceKind_Window,
						MumbleProto::VideoState_Codec_TiledImage);
	QCOMPARE(subscriptions.count(), 1);
	QCOMPARE(subscriptions.at(0).at(1).toUInt(), 11u);
	QVERIFY(subscriptions.at(0).at(2).toBool());
	QVERIFY(grid.surfaceFor(SENDER, 10).isNull());
	QImage small(32, 16, QImage::Format_RGB32);
	small.fill(Qt::red);
	QByteArray bytes;
	QBuffer buffer(&bytes);
	buffer.open(QIODevice::WriteOnly);
	small.save(&buffer, "JPEG");
	grid.onVideoUnitReceived(SENDER, 11, 0, true, 0, 0, bytes);
	waitForAsyncDecode();
	QCOMPARE(grid.surfaceFor(SENDER, 11).size(), QSize(32, 16));
}

// The grid cannot tell on its own that a sender has left the server - it only ever sees opaque session
// ids - so MainWindow reconciles it against the real user list and removes whatever belongs to a session
// that is gone (see MainWindow::pruneDepartedVideoSenders). That sweep is only as good as this list, and
// two properties of it matter: a sender holding several streams must appear once rather than once per
// stream, and an unwatched stream must appear at all - an unwatched tile is exactly the case no
// silence-based watchdog is allowed to clean up, so if it were missing here nothing would ever remove it.
void TestVideoGrid::senderSessionsReportsEachSessionOnceWatchedOrNot() {
	VideoGrid grid;

	QCOMPARE(grid.senderSessions().size(), std::size_t(0));

	// Two streams from one sender - a camera and a screen - and one of them left unwatched.
	grid.setStreamCodec(SENDER, 1, MumbleProto::VideoState_SourceKind_Camera, MumbleProto::VideoState_Codec_VP8);
	grid.setWatching(SENDER, 1, true);
	grid.setStreamCodec(SENDER, 2, MumbleProto::VideoState_SourceKind_Display,
						MumbleProto::VideoState_Codec_TiledImage);

	std::vector< unsigned int > sessions = grid.senderSessions();
	QCOMPARE(sessions.size(), std::size_t(1));
	QCOMPARE(sessions.front(), SENDER);

	// A second, different sender is its own entry.
	const unsigned int other = SENDER + 1;
	grid.setStreamCodec(other, 1, MumbleProto::VideoState_SourceKind_Camera, MumbleProto::VideoState_Codec_VP8);

	sessions = grid.senderSessions();
	QCOMPARE(sessions.size(), std::size_t(2));
	QVERIFY(std::find(sessions.begin(), sessions.end(), SENDER) != sessions.end());
	QVERIFY(std::find(sessions.begin(), sessions.end(), other) != sessions.end());

	// And dropping one sender's tiles - what the sweep itself does - takes every stream of theirs with it,
	// leaving the other sender untouched.
	grid.removeSender(SENDER);

	sessions = grid.senderSessions();
	QCOMPARE(sessions.size(), std::size_t(1));
	QCOMPARE(sessions.front(), other);
}

// Hover show/hide of the control bars must not happen inside Qt's own mouse and enter/leave dispatch.
// updateHoveredBar() shows and hides real QWidgets, and doing that while
// QApplicationPrivate::dispatchEnterLeave() is still walking the widgets it decided to notify mutates the
// hierarchy underneath that walk - which a Kubuntu tester hit as a hard SIGSEGV inside Qt's own
// invalidateGraphicsEffectsRecursively(), just from hovering the splitter beside the video panel. So the
// property under test is the timing itself: an enter event must leave the hover state alone until the
// event loop turns, and only then apply it.
void TestVideoGrid::hoverEventsDoNotTouchWidgetsInsideQtsOwnDispatch() {
	VideoGrid grid;
	grid.resize(640, 360);

	// One announced (unwatched, so it needs no decode) stream, which claims a cell and so gives the grid
	// a slot for the cursor to be over at all.
	grid.setStreamCodec(SENDER, STREAM, MumbleProto::VideoState_SourceKind_Camera,
						MumbleProto::VideoState_Codec_VP8);
	QCOMPARE(grid.senderCount(), 1);

	QCOMPARE(grid.hoveredSlotForTesting(), -1);

	// Delivered the way Qt delivers it - through the event system, not by calling enterEvent() directly -
	// so this exercises the real path the crash came in on.
	const QPointF inside(320.0, 180.0);
	QEnterEvent enter(inside, inside, grid.mapToGlobal(inside));
	QCoreApplication::sendEvent(&grid, &enter);

	// Still untouched: the widget work has been posted, not performed. This is the assertion that fails if
	// the handler ever goes back to calling updateHoveredBar() synchronously.
	QCOMPARE(grid.hoveredSlotForTesting(), -1);

	// And it does actually happen, one turn of the event loop later - deferring must not mean dropping.
	QCoreApplication::processEvents();
	QCOMPARE(grid.hoveredSlotForTesting(), 0);

	// Leaving is deferred the same way, and lands the same way.
	QEvent leave(QEvent::Leave);
	QCoreApplication::sendEvent(&grid, &leave);
	QCOMPARE(grid.hoveredSlotForTesting(), 0);

	QCoreApplication::processEvents();
	QCOMPARE(grid.hoveredSlotForTesting(), -1);

	// A burst of hovering - what dragging along the splitter actually produces - must coalesce rather than
	// pile up, and must leave the grid in one consistent state rather than crashing part-way through.
	for (int i = 0; i < 50; ++i) {
		QEnterEvent repeatedEnter(inside, inside, grid.mapToGlobal(inside));
		QCoreApplication::sendEvent(&grid, &repeatedEnter);

		QEvent repeatedLeave(QEvent::Leave);
		QCoreApplication::sendEvent(&grid, &repeatedLeave);
	}

	QCoreApplication::processEvents();

	// Last thing delivered was a leave, so nothing is hovered.
	QCOMPARE(grid.hoveredSlotForTesting(), -1);
}

// resizeEvent() calls relayout() unconditionally, and relayout() reaches the hover update - so dragging a
// splitter beside the video panel runs it on every resize tick, dozens a second. It used to hide every bar
// and re-show one on each of those, even though the hovered tile had not changed and only the geometry
// had. That is a lot of hide/show churn on widgets Qt is simultaneously busy resizing and repainting,
// which is the condition both crashes in this area came out of. So: a resize must do no visibility work.
void TestVideoGrid::resizingDoesNotChurnTheHoverBarsVisibility() {
	VideoGrid grid;
	grid.resize(640, 360);

	grid.setStreamCodec(SENDER, STREAM, MumbleProto::VideoState_SourceKind_Camera,
						MumbleProto::VideoState_Codec_VP8);

	// Settle the hover state first, so what follows measures resizing alone.
	const QPointF inside(320.0, 180.0);
	QEnterEvent enter(inside, inside, grid.mapToGlobal(inside));
	QCoreApplication::sendEvent(&grid, &enter);
	QCoreApplication::processEvents();

	const int settled         = grid.hoveredBarAppliedCountForTesting();
	const int settledRelayout = grid.relayoutControlsCallCountForTesting();
	QVERIFY(settled > 0);

	// A drag's worth of resizes. The event is sent explicitly as well as the widget actually being
	// resized: this grid is never shown, and an unshown widget does not get resize events delivered to it
	// on its own - without this the loop would exercise nothing at all and the assertion below would pass
	// for entirely the wrong reason.
	for (int i = 0; i < 50; ++i) {
		const QSize before = grid.size();
		grid.resize(640 - i, 360);

		QResizeEvent resize(grid.size(), before);
		QCoreApplication::sendEvent(&grid, &resize);
	}

	QCoreApplication::processEvents();

	// The resize path really did run - relayoutControls() is on the far side of relayout(), the same call
	// that reaches the hover update. Asserted so that this test cannot quietly become vacuous.
	QVERIFY(grid.relayoutControlsCallCountForTesting() > settledRelayout);

	// And not one of those resizes touched a bar's visibility: same hovered tile, same set of bars, only
	// the geometry moved.
	QCOMPARE(grid.hoveredBarAppliedCountForTesting(), settled);

	// But a real change still gets through - the early return must not be a way to go permanently stale.
	// A second stream is a structural change (a bar is created), which invalidates the decision.
	grid.setStreamCodec(SENDER, STREAM + 1, MumbleProto::VideoState_SourceKind_Display,
						MumbleProto::VideoState_Codec_TiledImage);
	QCoreApplication::processEvents();

	QVERIFY(grid.hoveredBarAppliedCountForTesting() > settled);
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
	waitForAsyncDecode();
	QCOMPARE(grid.surfaceFor(SENDER, 1).size(), QSize(64, 64));

	// No removeSender(SENDER, 1) in between: the end-of-stream never arrived.
	announce(grid, SENDER, 2, MumbleProto::VideoState_Codec_TiledImage, MumbleProto::VideoState_SourceKind_Camera);

	QVERIFY2(grid.surfaceFor(SENDER, 1).isNull(), "the stale camera surface survived a camera restart");
	QCOMPARE(grid.senderCount(), 0);

	grid.onVideoUnitReceived(SENDER, 2, 0, true, 0, 0, encoded);
	waitForAsyncDecode();
	QCOMPARE(grid.surfaceFor(SENDER, 2).size(), QSize(64, 64));
	QCOMPARE(grid.senderCount(), 1);
}
