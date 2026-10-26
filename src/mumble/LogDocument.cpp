// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "LogDocument.h"

#include <QtCore/QUrl>
#include <QtCore/QVector>
#include <QtGui/QImage>
#include <QtGui/QTextBlock>
#include <QtGui/QTextCursor>
#include <QtGui/QTextFormat>

LogDocument::LogDocument(QObject *p) : QTextDocument(p) {
}

QVariant LogDocument::loadResource(int type, const QUrl &url) {
	// Ignore requests for all external resources
	// that aren't images. We don't support any of them.
	if (type != QTextDocument::ImageResource) {
		addResource(type, url, QByteArray());
		return QByteArray();
	}

	// Only accept data URLs, not external resources. They are decoded here rather than by
	// QTextDocument::loadResource, which decodes whatever size the sender declared (bug 9: a few
	// messages of compressed blank pictures exhausted memory).
	if (url.isValid() && url.scheme() == QLatin1String("data")) {
		ChatImage::Result decoded = ChatImage::decodeDataUrl(url);
		if (decoded.verdict == ChatImage::Verdict::Ok) {
			const qint64 bytes = decoded.image.sizeInBytes();
			if (m_imageBytes + bytes <= ChatImage::MAX_DOCUMENT_IMAGE_BYTES) {
				m_imageBytes += bytes;
				addResource(type, url, decoded.image);
				return decoded.image;
			}
			decoded.verdict = ChatImage::Verdict::TooManyPixels;
		}
		m_rejectedImages.insert(url.toString(), decoded.verdict);
	}

	QImage qi(1, 1, QImage::Format_Mono);
	addResource(type, url, qi);

	return qi;
}

void LogDocument::clear() {
	QTextDocument::clear();
	// QTextDocument::clear() drops the cached resources too.
	m_imageBytes = 0;
	m_rejectedImages.clear();
}

bool LogDocument::isRejectedImage(const QString &resourceName, ChatImage::Verdict *verdict) const {
	auto it = m_rejectedImages.constFind(QUrl(resourceName).toString());
	if (it == m_rejectedImages.constEnd()) {
		return false;
	}
	if (verdict) {
		*verdict = it.value();
	}
	return true;
}

void LogDocument::sanitizeImages() {
	// Pictures: clamp the box each one declares, and replace the ones refused while loading with a note,
	// so that neither the log nor a tooltip rendering the returned HTML ever decodes them.
	struct ImageFix {
		int position;
		int length;
		QTextImageFormat format;
		bool rejected;
		ChatImage::Verdict verdict;
	};
	QVector< ImageFix > imageFixes;
	for (QTextBlock qtb = begin(); qtb != end(); qtb = qtb.next()) {
		for (QTextBlock::iterator qtbi = qtb.begin(); qtbi != qtb.end(); ++qtbi) {
			const QTextFragment &qtf = qtbi.fragment();
			if (!qtf.charFormat().isImageFormat()) {
				continue;
			}
			QTextImageFormat qtif = qtf.charFormat().toImageFormat();
			// Pictures with a declared size are not loaded by the layout; load them now so the checks run.
			(void) resource(QTextDocument::ImageResource, QUrl(qtif.name()));

			ChatImage::Verdict verdict = ChatImage::Verdict::Ok;
			if (isRejectedImage(qtif.name(), &verdict)) {
				imageFixes.append({ qtf.position(), qtf.length(), qtif, true, verdict });
				continue;
			}

			const QSizeF declared(qtif.hasProperty(QTextFormat::ImageWidth) ? qtif.width() : 0,
								  qtif.hasProperty(QTextFormat::ImageHeight) ? qtif.height() : 0);
			const QSizeF clamped = ChatImage::clampDeclaredSize(declared);
			if (clamped != declared) {
				if (clamped.width() > 0) {
					qtif.setWidth(clamped.width());
				} else {
					qtif.clearProperty(QTextFormat::ImageWidth);
				}
				if (clamped.height() > 0) {
					qtif.setHeight(clamped.height());
				} else {
					qtif.clearProperty(QTextFormat::ImageHeight);
				}
				imageFixes.append({ qtf.position(), qtf.length(), qtif, false, verdict });
			}
		}
	}
	// Back to front, so that replacing a picture with text does not move the positions still to come.
	for (auto it = imageFixes.crbegin(); it != imageFixes.crend(); ++it) {
		QTextCursor qtc(this);
		qtc.setPosition(it->position, QTextCursor::MoveAnchor);
		qtc.setPosition(it->position + it->length, QTextCursor::KeepAnchor);
		if (it->rejected) {
			qtc.insertText(ChatImage::placeholderText(it->verdict), QTextCharFormat());
		} else {
			qtc.setCharFormat(it->format);
		}
	}
}
