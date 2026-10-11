// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

// H.264 screen sharing: splitting a frame into transport units and joining it back, decoding, and - where
// this machine has a hardware encoder - a full round trip. CI runners have none, so the round trip skips
// there; the decoder is exercised against a fixed clip instead.

#include "H264Codec.h"
#include "VideoFragmentation.h"

#include <QObject>
#include <QtGui/QImage>
#include <QtTest>

#include <cstdint>
#include <vector>

namespace {

// Three access units of a 64x48 solid red clip (x264, baseline, no B-frames, SEI stripped): a keyframe
// with its parameter sets, then two inter-frames.
const std::uint8_t KEYFRAME[] = { 0x00, 0x00, 0x00, 0x01, 0x09, 0x10, 0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xc0,
								  0x0a, 0xd9, 0x04, 0x7b, 0x01, 0x10, 0x00, 0x00, 0x03, 0x00, 0x10, 0x00, 0x00,
								  0x03, 0x03, 0xc8, 0xf1, 0x22, 0x64, 0x80, 0x00, 0x00, 0x00, 0x01, 0x68, 0xcb,
								  0x83, 0xcb, 0x20, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x04, 0xbc, 0x46, 0x28,
								  0x00, 0x0a, 0x8b, 0xc7, 0x00, 0x01, 0x28, 0xd8, 0xe0, 0x00, 0x2f, 0xad, 0x27,
								  0x27, 0x27, 0x5d, 0x75, 0xd7, 0x5d, 0x75, 0xd7, 0x80 };
const std::uint8_t INTER1[]   = {
    0x00, 0x00, 0x00, 0x01, 0x09, 0x30, 0x00, 0x00, 0x01, 0x41, 0x9a, 0x38, 0x0b, 0xf8, 0xd8
};
const std::uint8_t INTER2[] = {
	0x00, 0x00, 0x00, 0x01, 0x09, 0x30, 0x00, 0x00, 0x01, 0x41, 0x9a, 0x54, 0x02, 0xfe, 0x36
};

QByteArray bytes(const std::uint8_t *data, std::size_t size) {
	return QByteArray(reinterpret_cast< const char * >(data), static_cast< int >(size));
}

QByteArray part(const EncodedVideoUnit &unit) {
	return QByteArray(reinterpret_cast< const char * >(unit.payload.data()), static_cast< int >(unit.payload.size()));
}

std::vector< std::uint8_t > pattern(std::size_t size) {
	std::vector< std::uint8_t > data(size);
	for (std::size_t i = 0; i < size; ++i) {
		data[i] = static_cast< std::uint8_t >(i * 7 + 3);
	}
	return data;
}

} // namespace

class TestH264Codec : public QObject {
	Q_OBJECT
private slots:
	void splitNumbersPartsInXAndY() {
		const std::vector< std::uint8_t > frame = pattern(2500);
		const auto units                        = H264Encoder::split(frame, 7, 42, 1000, true, 1920, 1080, 1000);

		QCOMPARE(units.size(), std::size_t(3));
		for (std::size_t i = 0; i < units.size(); ++i) {
			QCOMPARE(units[i].header.streamID, std::uint32_t(7));
			QCOMPARE(units[i].header.frameNumber, std::uint64_t(42));
			QCOMPARE(units[i].header.unitID, std::uint32_t(i));
			QCOMPARE(units[i].header.x, std::uint32_t(i));
			QCOMPARE(units[i].header.y, std::uint32_t(3));
			QVERIFY(units[i].header.isKeyframe);
			QCOMPARE(units[i].header.isFrameEnd, i == 2);
		}
		QCOMPARE(units[2].payload.size(), std::size_t(500));
	}

	void splitRefusesAFrameTooLargeToSend() {
		const std::vector< std::uint8_t > frame = pattern((H264Encoder::MAX_UNITS_PER_FRAME + 1) * 100);
		QVERIFY(H264Encoder::split(frame, 0, 0, 0, false, 64, 48, 100).empty());
	}

	void realUnitBudgetFitsTheTransport() {
		const std::size_t budget = Mumble::Protocol::VideoFragmenter::maxUnitSize();
		const auto units         = H264Encoder::split(pattern(budget * 3 + 1), 0, 0, 0, true, 64, 48, budget);

		QCOMPARE(units.size(), std::size_t(4));
		for (const EncodedVideoUnit &unit : units) {
			QVERIFY(unit.payload.size() <= budget);
		}
	}

	void assemblerJoinsPartsInAnyOrder() {
		const std::vector< std::uint8_t > frame = pattern(2500);
		const auto units                        = H264Encoder::split(frame, 0, 5, 0, true, 64, 48, 1000);

		H264FrameAssembler assembler;
		H264FrameAssembler::Complete out;

		QVERIFY(!assembler.add(5, true, 2, 3, part(units[2]), out));
		QVERIFY(!assembler.add(5, true, 0, 3, part(units[0]), out));
		QCOMPARE(assembler.missingParts(5), std::vector< unsigned int >{ 1 });
		QVERIFY(assembler.add(5, true, 1, 3, part(units[1]), out));

		QCOMPARE(out.frameNumber, std::uint64_t(5));
		QVERIFY(out.isKeyframe);
		QCOMPARE(out.accessUnit, QByteArray(reinterpret_cast< const char * >(frame.data()), 2500));

		// A late duplicate of a frame already handed on starts nothing.
		QVERIFY(!assembler.add(5, true, 0, 3, part(units[0]), out));
		QVERIFY(assembler.missingParts(5).empty());
	}

	void assemblerStillAcceptsAnOlderFrameAfterANewerOneCompletes() {
		// Frame 10 lost a part, frame 11 completed; the re-sent part of 10 must still complete it, because
		// that is how the decoding path recovers a gap.
		H264FrameAssembler assembler;
		H264FrameAssembler::Complete out;

		QVERIFY(!assembler.add(10, false, 0, 2, "a", out));
		QVERIFY(assembler.add(11, false, 0, 1, "b", out));
		QCOMPARE(out.frameNumber, std::uint64_t(11));

		QCOMPARE(assembler.missingParts(10), std::vector< unsigned int >{ 1 });
		QVERIFY(assembler.add(10, false, 1, 2, "c", out));
		QCOMPARE(out.frameNumber, std::uint64_t(10));
		QCOMPARE(out.accessUnit, QByteArray("ac"));
	}

	void assemblerIgnoresMalformedParts() {
		H264FrameAssembler assembler;
		H264FrameAssembler::Complete out;

		QVERIFY(!assembler.add(1, false, 0, 0, "x", out));
		QVERIFY(!assembler.add(1, false, 2, 2, "x", out));
		QVERIFY(!assembler.add(1, false, 0, H264Encoder::MAX_UNITS_PER_FRAME + 1, "x", out));
		QVERIFY(!assembler.add(1, false, 0, 2, QByteArray(), out));

		// A part claiming a different count than the frame's first part did is not trusted either.
		QVERIFY(!assembler.add(2, false, 0, 2, "x", out));
		QVERIFY(!assembler.add(2, false, 1, 3, "y", out));
		QCOMPARE(assembler.missingParts(2), std::vector< unsigned int >{ 1 });
	}

	void assemblerBoundsIncompleteFrames() {
		H264FrameAssembler assembler;
		H264FrameAssembler::Complete out;

		for (std::uint64_t frame = 0; frame < H264FrameAssembler::MAX_PENDING_FRAMES + 4; ++frame) {
			QVERIFY(!assembler.add(frame, false, 0, 2, "x", out));
		}

		// The oldest were let go; the newest are still waiting for their second part.
		QVERIFY(assembler.missingParts(0).empty());
		QCOMPARE(assembler.missingParts(H264FrameAssembler::MAX_PENDING_FRAMES + 3), std::vector< unsigned int >{ 1 });
	}

	void decodesAKeyframeAndWhatFollowsIt() {
		H264Decoder decoder;
		QVERIFY(decoder.isValid());

		const QImage key = decoder.decode(bytes(KEYFRAME, sizeof(KEYFRAME)));
		QVERIFY(!key.isNull());
		QCOMPARE(key.size(), QSize(64, 48));

		// Solid red, give or take the colour conversion.
		const QRgb centre = key.pixel(32, 24);
		QVERIFY(qRed(centre) > 200);
		QVERIFY(qGreen(centre) < 50);
		QVERIFY(qBlue(centre) < 50);

		QVERIFY(!decoder.decode(bytes(INTER1, sizeof(INTER1))).isNull());
		QVERIFY(!decoder.decode(bytes(INTER2, sizeof(INTER2))).isNull());
	}

	void showsNothingBeforeAKeyframe() {
		// A viewer who joins mid-stream must not be shown a picture predicted from frames it never had.
		H264Decoder decoder;
		QVERIFY(decoder.decode(bytes(INTER1, sizeof(INTER1))).isNull());
		QVERIFY(decoder.decode(bytes(INTER2, sizeof(INTER2))).isNull());

		QVERIFY(!decoder.decode(bytes(KEYFRAME, sizeof(KEYFRAME))).isNull());
	}

	void rejectsGarbage() {
		H264Decoder decoder;
		QVERIFY(decoder.decode(QByteArray()).isNull());
		QVERIFY(decoder.decode(QByteArray(500, '\x5a')).isNull());
		// Still usable afterwards.
		QVERIFY(!decoder.decode(bytes(KEYFRAME, sizeof(KEYFRAME))).isNull());
	}

	void roundTripsThroughTheHardwareEncoder() {
		if (!H264Encoder::isAvailable()) {
			QSKIP("no hardware H.264 encoder on this machine");
		}

		H264Encoder encoder;
		encoder.setBitrate(4000);
		encoder.setFramerate(30);

		H264Decoder decoder;
		H264FrameAssembler assembler;
		int decoded = 0;

		for (int frame = 0; frame < 30; ++frame) {
			QImage image(640, 360, QImage::Format_RGB32);
			image.fill(QColor(0, 0, 255));
			// A moving bar, so inter-frames carry real changes.
			for (int y = 0; y < 360; ++y) {
				for (int x = frame * 10; x < frame * 10 + 40 && x < 640; ++x) {
					image.setPixel(x, y, qRgb(255, 255, 0));
				}
			}

			for (const EncodedVideoUnit &unit : encoder.encode(image, 1, static_cast< std::uint64_t >(frame), 0)) {
				H264FrameAssembler::Complete out;
				if (assembler.add(unit.header.frameNumber, unit.header.isKeyframe, unit.header.x, unit.header.y,
								  part(unit), out)) {
					const QImage picture = decoder.decode(out.accessUnit);
					QVERIFY(!picture.isNull());
					QCOMPARE(picture.size(), QSize(640, 360));
					// Blue background survives the trip.
					QVERIFY(qBlue(picture.pixel(620, 10)) > 200);
					++decoded;
				}
			}
		}

		QVERIFY2(decoded >= 28, qPrintable(QString("only %1 of 30 frames decoded").arg(decoded)));
		QCOMPARE(encoder.stats().droppedOversize, 0u);
		qInfo("encoded with %s", qPrintable(encoder.backendName()));
	}
};

QTEST_MAIN(TestH264Codec)
#include "TestH264Codec.moc"
