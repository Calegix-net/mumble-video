// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "VideoPreview.h"

#include <QtGui/QPainter>

VideoPreview::VideoPreview(QWidget *parent) : QWidget(parent) {
	setMinimumSize(160, 120);
	setAutoFillBackground(true);
}

void VideoPreview::setFrame(const QImage &frame) {
	m_frame = frame;
	update();
}

void VideoPreview::clear() {
	m_frame = QImage();
	update();
}

void VideoPreview::paintEvent(QPaintEvent *) {
	QPainter painter(this);
	painter.fillRect(rect(), Qt::black);

	if (m_frame.isNull()) {
		painter.setPen(Qt::gray);
		painter.drawText(rect(), Qt::AlignCenter, tr("No preview"));

		return;
	}

	const QSize scaled = m_frame.size().scaled(size(), Qt::KeepAspectRatio);
	const QRect target((width() - scaled.width()) / 2, (height() - scaled.height()) / 2, scaled.width(),
					   scaled.height());

	painter.drawImage(target, m_frame);
}
