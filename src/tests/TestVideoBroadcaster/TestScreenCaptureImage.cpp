// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.
#include "ScreenCaptureImage.h"
#include <QtTest>
class TestScreenCaptureImage : public QObject {
	Q_OBJECT
private slots:
	void rotation() {
		QImage frame(3, 2, QImage::Format_ARGB32);
		frame.fill(Qt::black);
		frame.setPixel(0, 0, qRgb(255, 0, 0));
		frame.setPixel(2, 1, qRgb(0, 255, 0));
		auto portrait = ScreenCaptureImage::orient(frame, 2);
		QCOMPARE(portrait.size(), QSize(2, 3));
		QCOMPARE(portrait.pixel(1, 0), qRgb(255, 0, 0));
		QCOMPARE(portrait.pixel(0, 2), qRgb(0, 255, 0));
		QCOMPARE(ScreenCaptureImage::orient(portrait, 4), frame);
		auto flipped = ScreenCaptureImage::orient(frame, 3);
		QCOMPARE(flipped.pixel(2, 1), qRgb(255, 0, 0));
	}
	void colorPointerClipsAndBlends() {
		QImage frame(2, 1, QImage::Format_ARGB32);
		frame.fill(qRgb(0, 0, 200));
		const QRgb pixels[] = { qRgba(0, 255, 0, 255), qRgba(200, 0, 0, 128) };
		ScreenCaptureImage::drawPointer(frame, QPoint(-1, 0), 2, 2, 1, 8,
										reinterpret_cast< const unsigned char * >(pixels), sizeof(pixels));
		QCOMPARE(frame.pixel(0, 0), qRgb(100, 0, 100));
		QCOMPARE(frame.pixel(1, 0), qRgb(0, 0, 200));
	}
	void monochromePointer() {
		QImage frame(4, 1, QImage::Format_ARGB32);
		frame.fill(qRgb(10, 20, 30));
		const unsigned char masks[] = { 0x30, 0x50 }; // AND: 0011, XOR: 0101
		ScreenCaptureImage::drawPointer(frame, QPoint(0, 0), 1, 4, 2, 1, masks, sizeof(masks));
		QCOMPARE(frame.pixel(0, 0), qRgb(0, 0, 0));
		QCOMPARE(frame.pixel(1, 0), qRgb(255, 255, 255));
		QCOMPARE(frame.pixel(2, 0), qRgb(10, 20, 30));
		QCOMPARE(frame.pixel(3, 0), qRgb(245, 235, 225));
	}
	void maskedColorPointer() {
		QImage frame(2, 1, QImage::Format_ARGB32);
		frame.fill(qRgb(10, 20, 30));
		const QRgb pixels[] = { qRgba(100, 200, 50, 0), qRgba(255, 255, 255, 255) };
		ScreenCaptureImage::drawPointer(frame, QPoint(0, 0), 4, 2, 1, 8,
										reinterpret_cast< const unsigned char * >(pixels), sizeof(pixels));
		QCOMPARE(frame.pixel(0, 0), qRgb(100, 200, 50));
		QCOMPARE(frame.pixel(1, 0), qRgb(245, 235, 225));
	}
	void truncatedPointerIsIgnored() {
		QImage frame(2, 1, QImage::Format_ARGB32);
		frame.fill(Qt::blue);
		const QImage original      = frame;
		const unsigned char data[] = { 255 };
		ScreenCaptureImage::drawPointer(frame, QPoint(0, 0), 2, 2, 1, 8, data, sizeof(data));
		QCOMPARE(frame, original);
	}
};
QTEST_MAIN(TestScreenCaptureImage)
#include "TestScreenCaptureImage.moc"
