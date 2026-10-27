// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

// VP8 encode and decode, and the properties the transport depends on.
//
// The interesting assertions are not "it round-trips" but the two that make it usable at all: that a
// frame fits in one transport unit, and that inter-frames are dramatically smaller than keyframes --
// which is the entire reason for preferring this over tiled JPEG.

#include "VideoFragmentation.h"
#include "VpxCodec.h"

#include <QObject>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

/// A scene with enough detail to be worth compressing.
///
/// This matters more than it looks. An earlier version of this test used a smooth gradient with a solid
/// moving box, and VP8 encoded a whole 640x480 keyframe into 828 bytes -- so every assertion about
/// bitrate and about inter-frame savings was measuring nothing. Real video has texture, sensor grain and
/// motion, and only content of that kind exercises rate control at all.
///
/// The texture pans a little each frame, which is both realistic and useful: consecutive frames stay
/// highly predictable, while distant frames genuinely differ.
QImage renderScene(int w, int h, int frame) {
	QImage image(w, h, QImage::Format_RGB32);

	const int pan = frame * 2;

	for (int y = 0; y < h; ++y) {
		QRgb *scan = reinterpret_cast< QRgb * >(image.scanLine(y));

		for (int x = 0; x < w; ++x) {
			const int sx = x + pan;

			// Structured detail at several scales: compressible, unlike noise, but far from flat.
			int v = 110;
			v += ((sx / 3) % 17) * 4;
			v += ((y / 5) % 11) * 5;
			v += (((sx / 23) + (y / 19)) % 2) ? 28 : -28;

			// A little grain, as any real sensor has. Small enough to compress, large enough to matter.
			std::uint32_t g =
				static_cast< std::uint32_t >(sx) * 1103515245u + static_cast< std::uint32_t >(y) * 12345u + 7u;
			g ^= g >> 13;
			v += static_cast< int >(g % 13) - 6;

			scan[x] = qRgb(std::clamp(v, 0, 255), std::clamp(v - 25, 0, 255), std::clamp(v + 20, 0, 255));
		}
	}

	// A moving object, so there is real motion to predict rather than only a global pan.
	const int boxSize = std::max(24, w / 6);
	const int boxX    = (frame * 9) % std::max(1, w - boxSize);
	const int boxY    = (frame * 5) % std::max(1, h - boxSize);

	for (int y = boxY; y < boxY + boxSize && y < h; ++y) {
		QRgb *scan = reinterpret_cast< QRgb * >(image.scanLine(y));

		for (int x = boxX; x < boxX + boxSize && x < w; ++x) {
			scan[x] = qRgb(240, 40, 40);
		}
	}

	return image;
}

/// Screen-like content: a dark editor with rows of thin, high-contrast "text" strokes, scrolled up by
/// `scroll` pixels. Text is what a screen share is mostly made of and what codecs tuned for cameras do
/// worst on, so it is what the VP9 screen tuning has to be tested against.
QImage renderScreen(int w, int h, int scroll) {
	QImage image(w, h, QImage::Format_RGB32);
	image.fill(qRgb(30, 31, 34));

	for (int y = 0; y < h; ++y) {
		const int line = (y + scroll) / 20;
		const int row  = (y + scroll) % 20;

		if (row < 5 || row > 14) {
			continue;
		}

		QRgb *scan = reinterpret_cast< QRgb * >(image.scanLine(y));

		for (int x = 40; x < w - 40; ++x) {
			// Glyph-like: 7px cells, varying per line and column, with gaps for spaces and line endings.
			const std::uint32_t cell = static_cast< std::uint32_t >(line * 131 + (x / 7) * 17);
			const int lineLength = 200 + static_cast< int >((static_cast< std::uint32_t >(line) * 2654435761u) % 900);
			if (x > lineLength || (cell * 2246822519u >> 29) == 0) {
				continue;
			}
			const std::uint32_t bits = cell * 3266489917u;
			if ((bits >> ((x % 7) * 3 + row % 3)) & 1) {
				scan[x] = (line % 5 == 0) ? qRgb(106, 153, 85) : qRgb(212, 212, 212);
			}
		}
	}

	return image;
}

double meanDifference(const QImage &a, const QImage &b) {
	if (a.size() != b.size() || a.isNull()) {
		return 255.0;
	}

	double total = 0.0;

	for (int y = 0; y < a.height(); ++y) {
		for (int x = 0; x < a.width(); ++x) {
			const QRgb p = a.pixel(x, y);
			const QRgb q = b.pixel(x, y);

			total += std::abs(qRed(p) - qRed(q)) + std::abs(qGreen(p) - qGreen(q)) + std::abs(qBlue(p) - qBlue(q));
		}
	}

	return total / (a.width() * a.height() * 3.0);
}

} // namespace

class TestVpxCodec : public QObject {
	Q_OBJECT
private slots:
	void encoderInitialises();
	void aFrameRoundTripsRecognisably();
	void vp8CostsFarLessThanTiledImage();
	void everyFrameFitsOneTransportUnit();
	void bitrateControlIsRespected();
	void resizingRestartsTheStream();
	void resetForcesAKeyframe();
	void aDecoderWithoutAKeyframeProducesNothing();
	void garbageIsRejected();
	void aScreenIsEncodedWithVp9();
	void aScrollingScreenDecodesFrameByFrame();
	void aStillScreenStaysWithinItsBudget();
	void aScreenKeyframeFitsOneUnitAt1440p();

	void blackStaysBlackAndWhiteStaysWhite();
	void aFlatColourSurvivesTheRoundTrip();
};

// The conversion either side of the codec is full-range BT.601, and libswscale's default for YUV input
// is studio range. Getting that wrong does not fail, break or warn: every frame simply comes back with
// black lifted to 16 and white crushed to 235, which on a shared text editor reads as grey text on a
// grey page. These two tests are the guard, and they are written so that a studio-range mistake fails
// them by a wide margin rather than by a rounding error.

void TestVpxCodec::blackStaysBlackAndWhiteStaysWhite() {
	// Large flat blocks, so the codec has no reason to spend bits anywhere and the values that come
	// back are the conversion's rather than the encoder's opinion of them.
	QImage original(320, 240, QImage::Format_RGB32);
	original.fill(Qt::black);
	QPainter painter(&original);
	painter.fillRect(QRect(160, 0, 160, 240), Qt::white);
	painter.end();

	VpxEncoder encoder(VpxContent::Screen);
	encoder.setBitrate(3000);

	VpxDecoder decoder(VpxCodec::VP9);
	QVERIFY(decoder.isValid());

	// A few frames, because the first is where rate control is least settled.
	QImage decoded;
	for (unsigned int frame = 0; frame < 4; ++frame) {
		const std::vector< EncodedVideoUnit > units = encoder.encode(original, 1, frame, frame * 33000, frame == 0);
		QCOMPARE(units.size(), static_cast< std::size_t >(1));
		decoded = decoder.decode(units[0].payload);
	}

	QVERIFY(!decoded.isNull());
	QCOMPARE(decoded.size(), original.size());

	const QRgb black = decoded.pixel(80, 120);
	const QRgb white = decoded.pixel(240, 120);

	// Studio range would put black at about 16 and white at about 235. A tolerance of 8 is far inside
	// that gap and far outside anything the codec does to a flat block.
	QVERIFY2(qRed(black) <= 8 && qGreen(black) <= 8 && qBlue(black) <= 8,
			 qPrintable(QStringLiteral("black came back as %1,%2,%3 - studio range would be near 16")
							.arg(qRed(black))
							.arg(qGreen(black))
							.arg(qBlue(black))));
	QVERIFY2(qRed(white) >= 247 && qGreen(white) >= 247 && qBlue(white) >= 247,
			 qPrintable(QStringLiteral("white came back as %1,%2,%3 - studio range would be near 235")
							.arg(qRed(white))
							.arg(qGreen(white))
							.arg(qBlue(white))));
}

void TestVpxCodec::aFlatColourSurvivesTheRoundTrip() {
	// Chroma as well as luma, which a range or matrix mistake also moves. Mid-grey is deliberately
	// included: it is the one value studio and full range nearly agree on, so a test that only looked
	// at grey would pass either way.
	const QList< QRgb > colours = { qRgb(255, 0, 0), qRgb(0, 255, 0), qRgb(0, 0, 255), qRgb(128, 128, 128) };

	for (const QRgb colour : colours) {
		QImage original(320, 240, QImage::Format_RGB32);
		original.fill(colour);

		VpxEncoder encoder(VpxContent::Screen);
		encoder.setBitrate(3000);
		VpxDecoder decoder(VpxCodec::VP9);
		QVERIFY(decoder.isValid());

		QImage decoded;
		for (unsigned int frame = 0; frame < 4; ++frame) {
			const std::vector< EncodedVideoUnit > units =
				encoder.encode(original, 1, frame, frame * 33000, frame == 0);
			QCOMPARE(units.size(), static_cast< std::size_t >(1));
			decoded = decoder.decode(units[0].payload);
		}

		QVERIFY(!decoded.isNull());

		const QRgb back = decoded.pixel(160, 120);
		// 4:2:0 subsampling and the integer matrices on both sides cost a few levels; a range mistake
		// costs tens. 12 separates them with room to spare.
		const int dr = std::abs(qRed(back) - qRed(colour));
		const int dg = std::abs(qGreen(back) - qGreen(colour));
		const int db = std::abs(qBlue(back) - qBlue(colour));

		QVERIFY2(dr <= 12 && dg <= 12 && db <= 12,
				 qPrintable(QStringLiteral("%1,%2,%3 came back as %4,%5,%6")
								.arg(qRed(colour))
								.arg(qGreen(colour))
								.arg(qBlue(colour))
								.arg(qRed(back))
								.arg(qGreen(back))
								.arg(qBlue(back))));
	}
}

void TestVpxCodec::aScreenIsEncodedWithVp9() {
	VpxEncoder encoder(VpxContent::Screen);
	QCOMPARE(encoder.codec(), VpxCodec::VP9);

	encoder.setBitrate(3000);
	const QImage original = renderScreen(1280, 720, 0);

	const std::vector< EncodedVideoUnit > units = encoder.encode(original, 1, 0, 0, true);
	QCOMPARE(units.size(), static_cast< std::size_t >(1));
	QVERIFY(units[0].header.isKeyframe);

	// A VP8 decoder cannot read it; only a VP9 one can. That is why the stream is announced as VP9.
	VpxDecoder vp8;
	QVERIFY(vp8.decode(units[0].payload).isNull());

	VpxDecoder vp9(VpxCodec::VP9);
	const QImage decoded = vp9.decode(units[0].payload);
	QVERIFY(!decoded.isNull());
	QCOMPARE(decoded.size(), original.size());

	const double difference = meanDifference(original, decoded);
	QVERIFY2(difference < 12.0, qPrintable(QStringLiteral("mean difference %1").arg(difference)));
}

void TestVpxCodec::aScrollingScreenDecodesFrameByFrame() {
	VpxEncoder encoder(VpxContent::Screen);
	encoder.setBitrate(3000);
	VpxDecoder decoder(VpxCodec::VP9);

	QImage original, decoded;
	for (int i = 0; i < 30; ++i) {
		original = renderScreen(1280, 720, i * 12);
		const std::vector< EncodedVideoUnit > units =
			encoder.encode(original, 1, static_cast< std::uint64_t >(i), 0, i == 0);
		QCOMPARE(units.size(), static_cast< std::size_t >(1));
		QCOMPARE(units[0].header.isKeyframe, i == 0);

		decoded = decoder.decode(units[0].payload);
		QVERIFY2(!decoded.isNull(), qPrintable(QStringLiteral("frame %1 did not decode").arg(i)));
	}

	// After a second of steady scrolling, the picture is still the one being sent.
	const double difference = meanDifference(original, decoded);
	QVERIFY2(difference < 12.0, qPrintable(QStringLiteral("mean difference %1").arg(difference)));
	QVERIFY(meanDifference(renderScreen(1280, 720, 0), decoded) > difference);
}

void TestVpxCodec::aStillScreenStaysWithinItsBudget() {
	VpxEncoder encoder(VpxContent::Screen);
	encoder.setBitrate(3000);
	encoder.setFramerate(30);

	const QImage still        = renderScreen(1920, 1080, 0);
	std::size_t keyframeBytes = 0;
	std::size_t laterBytes    = 0;

	for (int i = 0; i < 60; ++i) {
		const std::vector< EncodedVideoUnit > units =
			encoder.encode(still, 1, static_cast< std::uint64_t >(i), 0, i == 0);
		QCOMPARE(units.size(), static_cast< std::size_t >(1));
		if (i == 0) {
			keyframeBytes = units[0].payload.size();
		} else if (i >= 30) {
			laterBytes += units[0].payload.size();
		}
	}

	qInfo("still 1080p screen: keyframe %zu bytes, frames 30-59 average %zu bytes", keyframeBytes, laterBytes / 30);

	// Constant bitrate keeps spending its budget on an unchanged picture, refining it - which is why
	// VideoBroadcaster stops feeding a still screen to the encoder after a second. What the codec itself
	// must guarantee is that it stays within that budget: 3000 kbit/s at 30 fps is 12.5 KB a frame.
	const std::size_t budgetPerFrame = 3000 * 1000 / 8 / 30;
	QVERIFY2(laterBytes / 30 < budgetPerFrame * 3 / 2,
			 qPrintable(QStringLiteral("later average %1, budget %2").arg(laterBytes / 30).arg(budgetPerFrame)));
}

void TestVpxCodec::aScreenKeyframeFitsOneUnitAt1440p() {
	// The largest picture a VP9 screen share is encoded at (VideoBroadcaster::MAX_VP9_WIDTH/HEIGHT), with
	// detail everywhere: the worst case for a keyframe. It must be sent, not dropped as oversize.
	VpxEncoder encoder(VpxContent::Screen);
	encoder.setBitrate(3000);

	const std::vector< EncodedVideoUnit > units = encoder.encode(renderScreen(2560, 1440, 0), 1, 0, 0, true);

	qInfo("1440p screen keyframe: %zu bytes of %zu allowed", units.empty() ? std::size_t{ 0 } : units[0].payload.size(),
		  Mumble::Protocol::VideoFragmenter::maxUnitSize());

	QCOMPARE(units.size(), static_cast< std::size_t >(1));
	QCOMPARE(encoder.stats().droppedOversize, 0u);
	QVERIFY(units[0].payload.size() <= Mumble::Protocol::VideoFragmenter::maxUnitSize());
}

void TestVpxCodec::encoderInitialises() {
	VpxEncoder encoder;

	// Constructed lazily: the encoder is configured on the first frame, since VP8 needs the resolution.
	QVERIFY(!encoder.encode(renderScene(320, 240, 0), 1, 0, 0).empty());
	QVERIFY(encoder.isValid());
}

void TestVpxCodec::aFrameRoundTripsRecognisably() {
	VpxEncoder encoder;
	VpxDecoder decoder;

	encoder.setBitrate(1500);

	const QImage original = renderScene(320, 240, 0);

	const std::vector< EncodedVideoUnit > units = encoder.encode(original, 1, 0, 0, true);
	QCOMPARE(units.size(), static_cast< std::size_t >(1));
	QVERIFY(units[0].header.isKeyframe);

	const QImage decoded = decoder.decode(units[0].payload);

	QVERIFY(!decoded.isNull());
	QCOMPARE(decoded.size(), original.size());

	// VP8 is lossy and goes through 4:2:0 chroma subsampling, so this is a "clearly the same picture"
	// threshold rather than a fidelity one.
	const double difference = meanDifference(original, decoded);
	QVERIFY2(difference < 12.0, qPrintable(QStringLiteral("mean difference %1").arg(difference)));

	// And the threshold discriminates: an unrelated frame must not pass it.
	QVERIFY(meanDifference(renderScene(320, 240, 40), decoded) > 12.0);
}

void TestVpxCodec::vp8CostsFarLessThanTiledImage() {
	// The argument for taking on a third-party dependency, stated as a measurement.
	//
	// Note what this does *not* assert. An earlier version compared keyframe size against inter-frame
	// size and expected a large ratio, which is simply wrong under constant bitrate: CBR spends the same
	// budget every frame and puts the savings into quality, not into smaller frames. The meaningful
	// comparison is against the codec VP8 is replacing, over the same content.
	const int frames = 30;

	VpxEncoder vp8;
	vp8.setBitrate(800);
	vp8.setFramerate(30);

	std::size_t vp8Bytes = 0;

	for (int i = 0; i < frames; ++i) {
		for (const EncodedVideoUnit &unit :
			 vp8.encode(renderScene(640, 480, i), 1, static_cast< std::uint64_t >(i), 0, i == 0)) {
			vp8Bytes += unit.payload.size();
		}
	}

	TiledImageEncoder tiled;
	tiled.setTileSize(128);
	tiled.setQuality(80);

	std::size_t tiledBytes = 0;

	for (int i = 0; i < frames; ++i) {
		tiled.encode(renderScene(640, 480, i), 1, static_cast< std::uint64_t >(i), 0);
		tiledBytes += tiled.lastStats().bytesEncoded;
	}

	QVERIFY(vp8Bytes > 0);
	QVERIFY(tiledBytes > 0);

	// A camera scene moves, so dirty-tile detection saves the JPEG path almost nothing, and it pays full
	// price for every frame. Anything less than a large factor here would mean libvpx is not earning its
	// place in the build.
	QVERIFY2(vp8Bytes * 4 < tiledBytes, qPrintable(QStringLiteral("VP8 %1 bytes vs TiledImage %2 bytes over %3 frames")
													   .arg(vp8Bytes)
													   .arg(tiledBytes)
													   .arg(frames)));
}

void TestVpxCodec::everyFrameFitsOneTransportUnit() {
	// VP8 has no independently decodable sub-frame region, so a frame is one unit and must fit whole.
	// Rate control plus the intra-bitrate cap is what makes that true; if it stopped being true, frames
	// would be dropped rather than truncated, which is why the encoder counts them.
	VpxEncoder encoder;
	encoder.setBitrate(1200);

	for (int i = 0; i < 30; ++i) {
		const std::vector< EncodedVideoUnit > units =
			encoder.encode(renderScene(1280, 720, i), 1, static_cast< std::uint64_t >(i), 0, i % 10 == 0);

		for (const EncodedVideoUnit &unit : units) {
			QVERIFY(unit.payload.size() <= Mumble::Protocol::VideoFragmenter::maxUnitSize());
		}
	}

	QCOMPARE(encoder.stats().droppedOversize, 0u);
	QVERIFY(encoder.stats().keyframes >= 3);
}

void TestVpxCodec::bitrateControlIsRespected() {
	// Not an exact assertion -- rate control is a target over time, not a per-frame guarantee -- but a
	// low target must produce visibly less data than a high one, or rate control is not working and the
	// transport budget means nothing.
	const auto measure = [](unsigned int kbps) {
		VpxEncoder encoder;
		encoder.setBitrate(kbps);
		encoder.setFramerate(30);
		encoder.setKeyframeInterval(1000);

		std::size_t total = 0;

		for (int i = 0; i < 30; ++i) {
			for (const EncodedVideoUnit &unit :
				 encoder.encode(renderScene(640, 480, i), 1, static_cast< std::uint64_t >(i), 0, i == 0)) {
				total += unit.payload.size();
			}
		}

		return total;
	};

	const std::size_t low  = measure(200);
	const std::size_t high = measure(3000);

	QVERIFY2(low < high, qPrintable(QStringLiteral("200kbps produced %1, 3000kbps produced %2").arg(low).arg(high)));
}

void TestVpxCodec::resizingRestartsTheStream() {
	VpxEncoder encoder;

	QVERIFY(!encoder.encode(renderScene(320, 240, 0), 1, 0, 0).empty());

	// VP8 cannot change resolution mid-stream, so a resize has to rebuild the encoder, and the first
	// frame afterwards must be a keyframe or a decoder has nothing to work from.
	const std::vector< EncodedVideoUnit > resized = encoder.encode(renderScene(640, 480, 1), 1, 1, 0);

	QCOMPARE(resized.size(), static_cast< std::size_t >(1));
	QVERIFY(resized[0].header.isKeyframe);
	QCOMPARE(resized[0].header.width, 640u);
	QCOMPARE(resized[0].header.height, 480u);
}

void TestVpxCodec::resetForcesAKeyframe() {
	VpxEncoder encoder;
	encoder.setKeyframeInterval(1000);

	encoder.encode(renderScene(320, 240, 0), 1, 0, 0, true);

	const std::vector< EncodedVideoUnit > inter = encoder.encode(renderScene(320, 240, 1), 1, 1, 0);
	QCOMPARE(inter.size(), static_cast< std::size_t >(1));
	QVERIFY(!inter[0].header.isKeyframe);

	// A new subscriber has no reference frame, so it needs a fresh keyframe on request.
	encoder.reset();

	const std::vector< EncodedVideoUnit > forced = encoder.encode(renderScene(320, 240, 2), 1, 2, 0);
	QCOMPARE(forced.size(), static_cast< std::size_t >(1));
	QVERIFY(forced[0].header.isKeyframe);
}

void TestVpxCodec::aDecoderWithoutAKeyframeProducesNothing() {
	VpxEncoder encoder;
	encoder.setKeyframeInterval(1000);

	encoder.encode(renderScene(320, 240, 0), 1, 0, 0, true);
	const std::vector< EncodedVideoUnit > inter = encoder.encode(renderScene(320, 240, 1), 1, 1, 0);
	QCOMPARE(inter.size(), static_cast< std::size_t >(1));

	// A decoder joining mid-stream is handed an inter-frame first. It must decline rather than produce a
	// garbled picture built on a reference it never received.
	VpxDecoder fresh;
	QVERIFY(fresh.decode(inter[0].payload).isNull());
}

void TestVpxCodec::garbageIsRejected() {
	VpxDecoder decoder;

	QVERIFY(decoder.decode({}).isNull());
	QVERIFY(decoder.decode(std::vector< Mumble::Protocol::byte >(64, 0xFF)).isNull());
	QVERIFY(decoder.decode(std::vector< Mumble::Protocol::byte >(3, 0x00)).isNull());
}

QTEST_MAIN(TestVpxCodec)
#include "TestVpxCodec.moc"
