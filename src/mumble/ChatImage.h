// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_CHATIMAGE_H_
#define MUMBLE_MUMBLE_CHATIMAGE_H_

#include <QtCore/QByteArray>
#include <QtCore/QSize>
#include <QtCore/QSizeF>
#include <QtCore/QString>
#include <QtCore/QUrl>
#include <QtGui/QImage>

/**
 * Decodes pictures embedded in chat messages, comments and channel descriptions without letting the
 * sender choose how much memory the receiver spends.
 *
 * Anyone in a channel can send HTML, and a few hundred KB of a compressed single-colour PNG declare tens
 * of thousands of pixels per side. Qt's own data: URL loader decodes such a picture at full size and the
 * log keeps every decoded picture until it is cleared, so a handful of messages exhausted memory and
 * crashed the client. Every limit here is checked before the pixels are allocated: the encoded size first,
 * then the dimensions from the image header, and the decode itself is scaled down to what the log shows.
 *
 * The server's imagemessagelength cannot be relied on: a misconfigured or malicious server relays
 * anything, so each client defends itself.
 */
namespace ChatImage {

/// Largest base64 payload accepted in a data: URL. Our own senders stay below it (see maxSendLength).
constexpr qsizetype MAX_ENCODED_BYTES = 1024 * 1024;
/// Largest decoded payload (what MAX_ENCODED_BYTES of base64 holds).
constexpr qsizetype MAX_DECODED_BYTES = MAX_ENCODED_BYTES / 4 * 3;
/// Largest width or height an image header may declare.
constexpr int MAX_SOURCE_SIDE = 20000;
/// Largest number of pixels an image header may declare. Decoders that cannot downscale while
/// decoding (PNG, GIF, progressive JPEG coefficients) allocate in proportion to this.
constexpr qint64 MAX_SOURCE_PIXELS = 40 * 1000 * 1000;
/// Decoded pictures are scaled so that neither side exceeds this.
constexpr int MAX_DISPLAY_SIDE = 2048;
/// Allocation limit handed to QImageReader, in MiB. Covers a full decode of MAX_SOURCE_PIXELS at 4 bytes.
constexpr int DECODE_ALLOCATION_LIMIT_MB = 160;
/// Total decoded image memory one document may hold. Pictures past it are shown as a placeholder
/// until the log is cleared.
constexpr qint64 MAX_DOCUMENT_IMAGE_BYTES = qint64(512) * 1024 * 1024;

enum class Verdict {
	Ok,
	NotDataUrl,
	EncodedTooLarge,
	UnsupportedFormat,
	BadHeader,
	TooManyPixels,
	DecodeFailed,
};

struct Result {
	QImage image;
	Verdict verdict = Verdict::DecodeFailed;
	/// Dimensions the header declared, when it could be read.
	QSize sourceSize;
};

/// Decodes raw image bytes within the limits above.
Result decode(const QByteArray &bytes);

/// Extracts and decodes the image in a data: URL within the limits above.
Result decodeDataUrl(const QUrl &url);

/// Extracts the payload of a data: URL, refusing it before decoding when it is larger than
/// MAX_ENCODED_BYTES of base64. Returns false and sets verdict when refused.
bool extractDataUrlPayload(const QUrl &url, QByteArray &payload, Verdict &verdict);

/// Clamps the width and height an <img> declared so the box it lays out stays within
/// MAX_DISPLAY_SIDE, keeping the aspect ratio when both were given. A value <= 0 or not finite means
/// "not declared" and comes back as 0.
QSizeF clampDeclaredSize(const QSizeF &declared);

/// The longest message our own client sends with a picture in it: the server's limit (0 = none)
/// bounded by what every one of our clients accepts.
int maxSendLength(int serverLimit);

/// Text shown in place of a picture that was refused.
QString placeholderText(Verdict verdict);

} // namespace ChatImage

#endif // MUMBLE_MUMBLE_CHATIMAGE_H_
