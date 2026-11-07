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
	// Smooth (bilinear) sampling rather than the default nearest-neighbour: the frame is scaled to the
	// widget's actual pixel size below, which is rarely its own resolution, and nearest-neighbour makes
	// that look blocky - the same fix applied to the remote-video surfaces in VideoGrid. A no-op at 1:1.
	painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
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
