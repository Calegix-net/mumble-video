// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "ChatImage.h"

#include <QtCore/QBuffer>
#include <QtCore/QCoreApplication>
#include <QtGui/QImageIOHandler>
#include <QtGui/QImageReader>

#include <algorithm>
#include <cmath>

namespace ChatImage {

namespace {
	bool isAllowedFormat(const QByteArray &format) {
		// Raster formats whose header carries the dimensions. SVG in particular is refused: a few bytes of
		// it ask for any size the sender likes, rendered at full resolution.
		static const QByteArray allowed[] = { "png", "jpeg", "jpg", "gif", "bmp", "webp" };
		return std::find(std::begin(allowed), std::end(allowed), format) != std::end(allowed);
	}

	bool isBase64Space(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }
} // namespace

Result decode(const QByteArray &bytes) {
	Result result;

	if (bytes.isEmpty()) {
		result.verdict = Verdict::BadHeader;
		return result;
	}
	if (bytes.size() > MAX_DECODED_BYTES) {
		result.verdict = Verdict::EncodedTooLarge;
		return result;
	}

	QBuffer buffer;
	buffer.setData(bytes);
	if (!buffer.open(QIODevice::ReadOnly)) {
		result.verdict = Verdict::DecodeFailed;
		return result;
	}

	QImageReader reader(&buffer);
	// Never trust the MIME type in the URL: pick the decoder from the bytes and check that one.
	reader.setDecideFormatFromContent(true);
	const QByteArray format = reader.format().toLower();
	if (!isAllowedFormat(format)) {
		result.verdict = Verdict::UnsupportedFormat;
		return result;
	}

	// Reads only the header.
	const QSize source = reader.size();
	result.sourceSize  = source;
	if (!source.isValid() || source.isEmpty()) {
		result.verdict = Verdict::BadHeader;
		return result;
	}
	if (source.width() > MAX_SOURCE_SIDE || source.height() > MAX_SOURCE_SIDE
		|| static_cast< qint64 >(source.width()) * source.height() > MAX_SOURCE_PIXELS) {
		result.verdict = Verdict::TooManyPixels;
		return result;
	}

	QSize target = source;
	if (target.width() > MAX_DISPLAY_SIDE || target.height() > MAX_DISPLAY_SIDE) {
		target = source.scaled(MAX_DISPLAY_SIDE, MAX_DISPLAY_SIDE, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
		// JPEG can scale while decoding (the DCT does it), so the full-size picture never exists. For the
		// others QImageReader would decode at full size and then scale, which we do below ourselves.
		if (reader.supportsOption(QImageIOHandler::ScaledSize)) {
			reader.setScaledSize(target);
		}
	}

	// A header that lies about its size still cannot make the decoder allocate more than this.
	reader.setAllocationLimit(DECODE_ALLOCATION_LIMIT_MB);

	QImage image = reader.read();
	if (image.isNull()) {
		result.verdict = Verdict::DecodeFailed;
		return result;
	}
	if (image.width() > MAX_DISPLAY_SIDE || image.height() > MAX_DISPLAY_SIDE) {
		image = image.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
	}

	result.image   = image;
	result.verdict = Verdict::Ok;
	return result;
}

bool extractDataUrlPayload(const QUrl &url, QByteArray &payload, Verdict &verdict) {
	if (!url.isValid() || url.scheme().compare(QLatin1String("data"), Qt::CaseInsensitive) != 0) {
		verdict = Verdict::NotDataUrl;
		return false;
	}

	// Each payload byte is at most "%XX" in the encoded form; refuse before decoding anything.
	const QString encoded = url.path(QUrl::FullyEncoded);
	if (encoded.size() > 3 * MAX_ENCODED_BYTES + 1024) {
		verdict = Verdict::EncodedTooLarge;
		return false;
	}

	const QByteArray raw = QByteArray::fromPercentEncoding(encoded.toLatin1());
	const qsizetype comma = raw.indexOf(',');
	if (comma < 0) {
		verdict = Verdict::BadHeader;
		return false;
	}

	const QByteArray header = raw.left(comma).toLower();
	const QByteArray data   = raw.mid(comma + 1);
	if (header.endsWith(";base64")) {
		const qsizetype significant =
			data.size() - std::count_if(data.cbegin(), data.cend(), [](char c) { return isBase64Space(c); });
		if (significant > MAX_ENCODED_BYTES) {
			verdict = Verdict::EncodedTooLarge;
			return false;
		}
		payload = QByteArray::fromBase64(data);
	} else {
		if (data.size() > MAX_DECODED_BYTES) {
			verdict = Verdict::EncodedTooLarge;
			return false;
		}
		payload = data;
	}

	verdict = Verdict::Ok;
	return true;
}

Result decodeDataUrl(const QUrl &url) {
	Result result;
	QByteArray payload;
	if (!extractDataUrlPayload(url, payload, result.verdict)) {
		return result;
	}
	return decode(payload);
}

QSizeF clampDeclaredSize(const QSizeF &declared) {
	auto sane = [](qreal v) { return std::isfinite(v) && v > 0 ? v : 0.0; };
	qreal w = sane(declared.width());
	qreal h = sane(declared.height());

	const qreal limit = MAX_DISPLAY_SIDE;
	if (w > 0 && h > 0) {
		const qreal factor = std::min({ qreal(1), limit / w, limit / h });
		w                  = std::max(qreal(1), w * factor);
		h                  = std::max(qreal(1), h * factor);
	} else {
		w = std::min(w, limit);
		h = std::min(h, limit);
	}
	return QSizeF(w, h);
}

int maxSendLength(int serverLimit) {
	// imageToImg's output is the base64 plus percent-encoding and line breaks, so a message within this
	// length always carries less than MAX_ENCODED_BYTES of base64.
	constexpr int ownLimit = static_cast< int >(MAX_ENCODED_BYTES);
	return serverLimit > 0 ? std::min(serverLimit, ownLimit) : ownLimit;
}

QString placeholderText(Verdict verdict) {
	switch (verdict) {
		case Verdict::EncodedTooLarge:
		case Verdict::TooManyPixels:
			return QCoreApplication::translate("ChatImage", "[[ Image too large to display ]]");
		case Verdict::UnsupportedFormat:
			return QCoreApplication::translate("ChatImage", "[[ Unsupported image format ]]");
		default:
			return QCoreApplication::translate("ChatImage", "[[ Invalid image ]]");
	}
}

} // namespace ChatImage
