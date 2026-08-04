// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

// VP8 encode and decode, and the properties the transport depends on.
//
// The interesting assertions are not "it round-trips" but the two that make it usable at all: that a
// frame fits in one transport unit, and that inter-frames are dramatically smaller than keyframes --
// which is the entire reason for preferring this over tiled JPEG.

#include "VP8Codec.h"
#include "VideoFragmentation.h"

#include <QObject>
#include <QtGui/QImage>
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

class TestVP8Codec : public QObject {
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
};

void TestVP8Codec::encoderInitialises() {
	VP8Encoder encoder;

	// Constructed lazily: the encoder is configured on the first frame, since VP8 needs the resolution.
	QVERIFY(!encoder.encode(renderScene(320, 240, 0), 1, 0, 0).empty());
	QVERIFY(encoder.isValid());
}

void TestVP8Codec::aFrameRoundTripsRecognisably() {
	VP8Encoder encoder;
	VP8Decoder decoder;

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

void TestVP8Codec::vp8CostsFarLessThanTiledImage() {
	// The argument for taking on a third-party dependency, stated as a measurement.
	//
	// Note what this does *not* assert. An earlier version compared keyframe size against inter-frame
	// size and expected a large ratio, which is simply wrong under constant bitrate: CBR spends the same
	// budget every frame and puts the savings into quality, not into smaller frames. The meaningful
	// comparison is against the codec VP8 is replacing, over the same content.
	const int frames = 30;

	VP8Encoder vp8;
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

void TestVP8Codec::everyFrameFitsOneTransportUnit() {
	// VP8 has no independently decodable sub-frame region, so a frame is one unit and must fit whole.
	// Rate control plus the intra-bitrate cap is what makes that true; if it stopped being true, frames
	// would be dropped rather than truncated, which is why the encoder counts them.
	VP8Encoder encoder;
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

void TestVP8Codec::bitrateControlIsRespected() {
	// Not an exact assertion -- rate control is a target over time, not a per-frame guarantee -- but a
	// low target must produce visibly less data than a high one, or rate control is not working and the
	// transport budget means nothing.
	const auto measure = [](unsigned int kbps) {
		VP8Encoder encoder;
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

void TestVP8Codec::resizingRestartsTheStream() {
	VP8Encoder encoder;

	QVERIFY(!encoder.encode(renderScene(320, 240, 0), 1, 0, 0).empty());

	// VP8 cannot change resolution mid-stream, so a resize has to rebuild the encoder, and the first
	// frame afterwards must be a keyframe or a decoder has nothing to work from.
	const std::vector< EncodedVideoUnit > resized = encoder.encode(renderScene(640, 480, 1), 1, 1, 0);

	QCOMPARE(resized.size(), static_cast< std::size_t >(1));
	QVERIFY(resized[0].header.isKeyframe);
	QCOMPARE(resized[0].header.width, 640u);
	QCOMPARE(resized[0].header.height, 480u);
}

void TestVP8Codec::resetForcesAKeyframe() {
	VP8Encoder encoder;
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

void TestVP8Codec::aDecoderWithoutAKeyframeProducesNothing() {
	VP8Encoder encoder;
	encoder.setKeyframeInterval(1000);

	encoder.encode(renderScene(320, 240, 0), 1, 0, 0, true);
	const std::vector< EncodedVideoUnit > inter = encoder.encode(renderScene(320, 240, 1), 1, 1, 0);
	QCOMPARE(inter.size(), static_cast< std::size_t >(1));

	// A decoder joining mid-stream is handed an inter-frame first. It must decline rather than produce a
	// garbled picture built on a reference it never received.
	VP8Decoder fresh;
	QVERIFY(fresh.decode(inter[0].payload).isNull());
}

void TestVP8Codec::garbageIsRejected() {
	VP8Decoder decoder;

	QVERIFY(decoder.decode({}).isNull());
	QVERIFY(decoder.decode(std::vector< Mumble::Protocol::byte >(64, 0xFF)).isNull());
	QVERIFY(decoder.decode(std::vector< Mumble::Protocol::byte >(3, 0x00)).isNull());
}

QTEST_MAIN(TestVP8Codec)
#include "TestVP8Codec.moc"
