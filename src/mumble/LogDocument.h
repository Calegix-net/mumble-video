// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_LOGDOCUMENT_H_
#define MUMBLE_MUMBLE_LOGDOCUMENT_H_

#include "ChatImage.h"

#include <QtCore/QHash>
#include <QtCore/QString>
#include <QtGui/QTextDocument>

/**
 * The document behind the chat log, the message editor and Log::validHtml. It loads no external
 * resources, and decodes data: URL pictures only within the ChatImage limits.
 */
class LogDocument : public QTextDocument {
private:
	Q_OBJECT
	Q_DISABLE_COPY(LogDocument)

	/// Pictures this document refused, by resource name, and why.
	QHash< QString, ChatImage::Verdict > m_rejectedImages;
	/// Decoded image memory this document holds (see ChatImage::MAX_DOCUMENT_IMAGE_BYTES).
	qint64 m_imageBytes = 0;

public:
	LogDocument(QObject *p = nullptr);
	QVariant loadResource(int, const QUrl &) Q_DECL_OVERRIDE;
	void clear() Q_DECL_OVERRIDE;

	/// Whether the picture named resourceName was refused; verdict says why.
	bool isRejectedImage(const QString &resourceName, ChatImage::Verdict *verdict = nullptr) const;

	/// Decoded image memory this document holds.
	qint64 imageBytes() const { return m_imageBytes; }

	/// Loads every picture in the document, replaces the refused ones with a note and clamps the box each
	/// of the others declares. Afterwards toHtml() names no picture this document refused, so whatever
	/// renders that HTML (the log, a tooltip) never decodes one.
	void sanitizeImages();
};

#endif // MUMBLE_MUMBLE_LOGDOCUMENT_H_
