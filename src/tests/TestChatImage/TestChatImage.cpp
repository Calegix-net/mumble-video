// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

// Pictures in chat HTML, which any user in a channel can send to everyone else in it.
//
// A few hundred KB of a compressed blank PNG declare thousands of pixels per side. The client used to
// decode them at full size and keep every one in the log, so a handful of messages exhausted memory
// and crashed it (bug 9). These tests send the same kind of message through the documents the log uses
// and check that the client stays within a fixed memory budget, while ordinary pictures still show.

#include "ChatImage.h"
#include "LogDocument.h"

#include <QtCore/QBuffer>
#include <QtCore/QByteArray>
#include <QtGui/QImage>
#include <QtGui/QImageWriter>
#include <QtGui/QPainter>
#include <QtGui/QTextCursor>
#include <QtGui/QTextDocumentFragment>
#include <QtTest>

#ifdef Q_OS_LINUX
#	include <sys/resource.h>
#endif

namespace {

QByteArray encode(const QImage &image, const char *format, int quality = -1) {
	QByteArray bytes;
	QBuffer buffer(&bytes);
	buffer.open(QIODevice::WriteOnly);
	QImageWriter writer(&buffer, format);
	if (quality >= 0) {
		writer.setQuality(quality);
	}
	writer.write(image);
	return bytes;
}

quint32 crc32(const QByteArray &data) {
	quint32 crc = 0xFFFFFFFFu;
	for (char ch : data) {
		crc ^= static_cast< quint8 >(ch);
		for (int k = 0; k < 8; ++k) {
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
		}
	}
	return ~crc;
}

void putBE32(QByteArray &bytes, int at, quint32 v) {
	bytes[at]     = static_cast< char >(v >> 24);
	bytes[at + 1] = static_cast< char >(v >> 16);
	bytes[at + 2] = static_cast< char >(v >> 8);
	bytes[at + 3] = static_cast< char >(v);
}

void putBE16(QByteArray &bytes, int at, quint16 v) {
	bytes[at]     = static_cast< char >(v >> 8);
	bytes[at + 1] = static_cast< char >(v);
}

void putLE16(QByteArray &bytes, int at, quint16 v) {
	bytes[at]     = static_cast< char >(v);
	bytes[at + 1] = static_cast< char >(v >> 8);
}

/// A real small PNG whose header then claims width x height.
QByteArray pngClaiming(quint32 width, quint32 height) {
	QImage small(8, 8, QImage::Format_RGB32);
	small.fill(Qt::white);
	QByteArray png = encode(small, "png");
	// Signature (8), IHDR length (4), "IHDR" (4), then width and height.
	putBE32(png, 16, width);
	putBE32(png, 20, height);
	putBE32(png, 29, crc32(png.mid(12, 17)));
	return png;
}

/// A real small baseline JPEG whose frame header then claims width x height.
QByteArray jpegClaiming(quint16 width, quint16 height) {
	QImage small(16, 16, QImage::Format_RGB32);
	small.fill(Qt::white);
	QByteArray jpeg = encode(small, "jpeg");
	const qsizetype sof = jpeg.indexOf(QByteArray("\xFF\xC0", 2));
	if (sof < 0) {
		return QByteArray();
	}
	// Marker (2), length (2), precision (1), then height and width.
	putBE16(jpeg, static_cast< int >(sof) + 5, height);
	putBE16(jpeg, static_cast< int >(sof) + 7, width);
	return jpeg;
}

/// A 1x1 GIF whose screen and image descriptor then claim width x height.
QByteArray gifClaiming(quint16 width, quint16 height) {
	static const char gif[] = "GIF89a\x01\x00\x01\x00\x80\x00\x00\xff\xff\xff\x00\x00\x00"
							  "!\xf9\x04\x01\x00\x00\x00\x00"
							  ",\x00\x00\x00\x00\x01\x00\x01\x00\x00\x02\x02\x44\x01\x00;";
	QByteArray bytes(gif, sizeof(gif) - 1);
	putLE16(bytes, 6, width);
	putLE16(bytes, 8, height);
	const qsizetype descriptor = bytes.indexOf(',');
	putLE16(bytes, static_cast< int >(descriptor) + 5, width);
	putLE16(bytes, static_cast< int >(descriptor) + 7, height);
	return bytes;
}

/// The attack: a blank 8000x8000 PNG, which compresses to a few dozen KB.
QByteArray blankPngBomb() {
	QImage bomb(8000, 8000, QImage::Format_Indexed8);
	bomb.setColorTable({ qRgb(255, 255, 255) });
	bomb.fill(0);
	return encode(bomb, "png");
}

QString dataUrl(const QByteArray &bytes, const char *mime = "image/png") {
	return QString::fromLatin1("data:%1;base64,%2").arg(QLatin1String(mime), QString::fromLatin1(bytes.toBase64()));
}

QString imgMessage(const QString &url, const QString &attributes = QString()) {
	return QString::fromLatin1("<p>look</p><img %1 src=\"%2\" />").arg(attributes, url);
}

/// What Log::validHtml and Log::log do with a received message: lay it out in a scratch document,
/// sanitize it, then copy it into the log.
void receive(LogDocument &log, const QString &html) {
	LogDocument scratch;
	scratch.setTextWidth(960);
	(void) scratch.documentLayout();
	scratch.setHtml(html);
	scratch.sanitizeImages();

	QTextCursor all(&scratch);
	all.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
	(void) log.documentLayout();
	QTextCursor end(&log);
	end.movePosition(QTextCursor::End);
	end.insertFragment(all.selection());
}

/// Paints the whole log, which is where Qt decodes pictures that declare their own size.
void paint(LogDocument &log) {
	log.setTextWidth(960);
	QImage canvas(960, 2000, QImage::Format_ARGB32_Premultiplied);
	QPainter painter(&canvas);
	log.drawContents(&painter);
}

qint64 peakRssMiB() {
#ifdef Q_OS_LINUX
	struct rusage usage;
	getrusage(RUSAGE_SELF, &usage);
	return usage.ru_maxrss / 1024;
#else
	return -1;
#endif
}

} // namespace

class TestChatImage : public QObject {
	Q_OBJECT
private slots:
	void headersClaimingTooManyPixelsAreRefused_data();
	void headersClaimingTooManyPixelsAreRefused();
	void gifClaimingHugeScreenIsRefused();
	void svgIsRefused();
	void oversizedPayloadIsRefusedBeforeDecoding();
	void normalPictureIsUnchanged();
	void largePhotoIsDownscaledOnDecode();
	void largePngIsDownscaled();
	void declaredSizeIsClamped();
	void sendLengthNeverExceedsWhatWeAccept();
	void refusedPictureBecomesTextInHtml();
	void declaredSizeIsClampedInHtml();
	void floodOfBombsStaysWithinMemoryBudget();
	void floodOfLegitPicturesStaysWithinDocumentBudget();
	void clearResetsBudget();
};

void TestChatImage::headersClaimingTooManyPixelsAreRefused_data() {
	QTest::addColumn< QByteArray >("bytes");
	QTest::newRow("png 8000x8000") << pngClaiming(8000, 8000);
	QTest::newRow("png 30000x10") << pngClaiming(30000, 10);
	QTest::newRow("png 2^31-1 square") << pngClaiming(0x7FFFFFFF, 0x7FFFFFFF);
	QTest::newRow("jpeg 60000x60000") << jpegClaiming(60000, 60000);
	QTest::newRow("jpeg 21000x100") << jpegClaiming(21000, 100);
}

void TestChatImage::headersClaimingTooManyPixelsAreRefused() {
	QFETCH(QByteArray, bytes);
	QVERIFY(!bytes.isEmpty());
	const ChatImage::Result result = ChatImage::decode(bytes);
	// Refused from the header alone: nothing was decoded.
	QVERIFY(result.verdict == ChatImage::Verdict::TooManyPixels || result.verdict == ChatImage::Verdict::BadHeader);
	QVERIFY(result.image.isNull());
}

void TestChatImage::gifClaimingHugeScreenIsRefused() {
	const ChatImage::Result result = ChatImage::decode(gifClaiming(65535, 65535));
	QVERIFY(result.verdict != ChatImage::Verdict::Ok);
	QVERIFY(result.image.isNull());
}

void TestChatImage::svgIsRefused() {
	const QByteArray svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"30000\" height=\"30000\">"
						   "<rect width=\"100%\" height=\"100%\" fill=\"white\"/></svg>";
	const ChatImage::Result result = ChatImage::decodeDataUrl(QUrl(dataUrl(svg, "image/png")));
	QCOMPARE(result.verdict, ChatImage::Verdict::UnsupportedFormat);
	QVERIFY(result.image.isNull());
}

void TestChatImage::oversizedPayloadIsRefusedBeforeDecoding() {
	QByteArray payload = pngClaiming(8, 8);
	payload.append(QByteArray(ChatImage::MAX_DECODED_BYTES, '\0'));
	const ChatImage::Result result = ChatImage::decodeDataUrl(QUrl(dataUrl(payload)));
	QCOMPARE(result.verdict, ChatImage::Verdict::EncodedTooLarge);

	// Non-base64 data URLs are bounded too.
	const QString raw =
		QString::fromLatin1("data:image/png,") + QString(ChatImage::MAX_DECODED_BYTES + 1, QLatin1Char('A'));
	QCOMPARE(ChatImage::decodeDataUrl(QUrl(raw)).verdict, ChatImage::Verdict::EncodedTooLarge);

	QCOMPARE(ChatImage::decodeDataUrl(QUrl(QString::fromLatin1("https://example.com/a.png"))).verdict,
			 ChatImage::Verdict::NotDataUrl);
}

void TestChatImage::normalPictureIsUnchanged() {
	// What our own clients send: at most 1600x1000 JPEG.
	QImage photo(1600, 1000, QImage::Format_RGB32);
	photo.fill(QColor(30, 120, 200));
	const QByteArray jpeg = encode(photo, "jpeg", 85);

	// Mumble's own encoder splits the base64 into percent-encoded lines; that must still decode.
	QByteArray base64 = jpeg.toBase64();
	QByteArray wrapped;
	for (qsizetype i = 0; i < base64.size(); i += 72) {
		wrapped.append(QUrl::toPercentEncoding(QLatin1String(base64.mid(i, 72))));
		wrapped.append('\n');
	}
	const ChatImage::Result result =
		ChatImage::decodeDataUrl(QUrl(QString::fromLatin1("data:image/jpeg;base64,") + QLatin1String(wrapped)));
	QCOMPARE(result.verdict, ChatImage::Verdict::Ok);
	QCOMPARE(result.image.size(), QSize(1600, 1000));
}

void TestChatImage::largePhotoIsDownscaledOnDecode() {
	QImage photo(6000, 4000, QImage::Format_RGB32);
	photo.fill(Qt::darkGreen);
	const ChatImage::Result result = ChatImage::decode(encode(photo, "jpeg", 70));
	QCOMPARE(result.verdict, ChatImage::Verdict::Ok);
	QCOMPARE(result.sourceSize, QSize(6000, 4000));
	QCOMPARE(result.image.width(), ChatImage::MAX_DISPLAY_SIDE);
	QCOMPARE(result.image.height(), 1365);
}

void TestChatImage::largePngIsDownscaled() {
	QImage shot(3000, 6000, QImage::Format_RGB32);
	shot.fill(Qt::gray);
	const ChatImage::Result result = ChatImage::decode(encode(shot, "png"));
	QCOMPARE(result.verdict, ChatImage::Verdict::Ok);
	QCOMPARE(result.image.height(), ChatImage::MAX_DISPLAY_SIDE);
	QCOMPARE(result.image.width(), 1024);
}

void TestChatImage::declaredSizeIsClamped() {
	QCOMPARE(ChatImage::clampDeclaredSize(QSizeF(100000, 100000)), QSizeF(2048, 2048));
	QCOMPARE(ChatImage::clampDeclaredSize(QSizeF(400, 300)), QSizeF(400, 300));
	QCOMPARE(ChatImage::clampDeclaredSize(QSizeF(8192, 1024)), QSizeF(2048, 256));
	QCOMPARE(ChatImage::clampDeclaredSize(QSizeF(0, 1e12)), QSizeF(0, 2048));
	QCOMPARE(ChatImage::clampDeclaredSize(QSizeF(1e9, 10)), QSizeF(2048, 1));
	QCOMPARE(ChatImage::clampDeclaredSize(QSizeF(-5, qQNaN())), QSizeF(0, 0));
	QCOMPARE(ChatImage::clampDeclaredSize(QSizeF(qInf(), 50)), QSizeF(0, 50));
}

void TestChatImage::sendLengthNeverExceedsWhatWeAccept() {
	QCOMPARE(ChatImage::maxSendLength(131072), 131072);
	QCOMPARE(ChatImage::maxSendLength(0), static_cast< int >(ChatImage::MAX_ENCODED_BYTES));
	QCOMPARE(ChatImage::maxSendLength(64 * 1024 * 1024), static_cast< int >(ChatImage::MAX_ENCODED_BYTES));
}

void TestChatImage::refusedPictureBecomesTextInHtml() {
	LogDocument doc;
	(void) doc.documentLayout();
	doc.setHtml(imgMessage(dataUrl(pngClaiming(8000, 8000))));
	doc.sanitizeImages();
	const QString html = doc.toHtml();
	// A tooltip rendering this HTML would otherwise decode the picture itself, without our limits.
	QVERIFY(!html.contains(QLatin1String("data:")));
	QVERIFY(doc.toPlainText().contains(ChatImage::placeholderText(ChatImage::Verdict::TooManyPixels)));
}

void TestChatImage::declaredSizeIsClampedInHtml() {
	QImage small(4, 4, QImage::Format_RGB32);
	small.fill(Qt::red);
	LogDocument doc;
	doc.setTextWidth(960);
	(void) doc.documentLayout();
	doc.setHtml(imgMessage(dataUrl(encode(small, "png")), QLatin1String("width=\"100000\" height=\"50000\"")));
	doc.sanitizeImages();
	doc.adjustSize();
	QVERIFY(doc.toHtml().contains(QLatin1String("width=\"2048\"")));
	QVERIFY(doc.toHtml().contains(QLatin1String("height=\"1024\"")));
	QVERIFY(doc.size().width() < 2100);
}

void TestChatImage::floodOfBombsStaysWithinMemoryBudget() {
	const QByteArray bomb = blankPngBomb();
	// Fits easily under the default imagemessagelength of the server.
	QVERIFY(bomb.toBase64().size() < 131072);

	LogDocument log;
	const qint64 before = peakRssMiB();
	// Unpatched, each of these kept a 256 MB picture in the log: 32 of them is 8 GB.
	for (int i = 0; i < 32; ++i) {
		const QString url = dataUrl(bomb) + QString::fromLatin1("#%1").arg(i);
		receive(log, imgMessage(url, QLatin1String("width=\"2000\" height=\"2000\"")));
		paint(log);
	}
	const qint64 after = peakRssMiB();

	if (before >= 0) {
		qInfo("peak RSS %lld MiB -> %lld MiB", before, after);
		QVERIFY2(after - before < 128, "the flood grew the client's memory");
	}
	QCOMPARE(log.imageBytes(), qint64(0));
	QVERIFY(log.toPlainText().contains(ChatImage::placeholderText(ChatImage::Verdict::TooManyPixels)));
}

void TestChatImage::floodOfLegitPicturesStaysWithinDocumentBudget() {
	// Within every per-picture limit, but each decodes to a 2048x2048 picture (16 MiB).
	QImage shot(4000, 4000, QImage::Format_Grayscale8);
	shot.fill(200);
	const QByteArray png = encode(shot, "png");

	LogDocument log;
	const qint64 before = peakRssMiB();
	for (int i = 0; i < 48; ++i) {
		receive(log, imgMessage(dataUrl(png) + QString::fromLatin1("#%1").arg(i)));
	}
	const qint64 after = peakRssMiB();

	QVERIFY(log.imageBytes() > 0);
	QVERIFY(log.imageBytes() <= ChatImage::MAX_DOCUMENT_IMAGE_BYTES);
	if (before >= 0) {
		qInfo("peak RSS %lld MiB -> %lld MiB", before, after);
		// The log's budget, plus one scratch document's worth of decoding.
		QVERIFY(after - before < ChatImage::MAX_DOCUMENT_IMAGE_BYTES / (1024 * 1024) + 256);
	}
}

void TestChatImage::clearResetsBudget() {
	QImage small(64, 64, QImage::Format_RGB32);
	small.fill(Qt::blue);
	LogDocument log;
	receive(log, imgMessage(dataUrl(encode(small, "png"))));
	QVERIFY(log.imageBytes() > 0);
	log.clear();
	QCOMPARE(log.imageBytes(), qint64(0));
}

QTEST_MAIN(TestChatImage)
#include "TestChatImage.moc"
