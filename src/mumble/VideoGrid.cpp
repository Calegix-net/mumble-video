// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "VideoGrid.h"

#include <QtGui/QPainter>

#include <algorithm>
#include <cmath>

VideoGrid::VideoGrid(QWidget *parent) : QWidget(parent) {
	setAutoFillBackground(true);
}

QImage VideoGrid::surfaceFor(unsigned int senderSession) const {
	const auto it = m_surfaces.find(senderSession);

	return it == m_surfaces.end() ? QImage() : it->canvas;
}

bool VideoGrid::growToFit(QImage &canvas, int x, int y, int width, int height) {
	if (width <= 0 || height <= 0) {
		return false;
	}

	// Checked in int arithmetic that cannot overflow, because these values come off the network.
	if (x < 0 || y < 0 || x > MAX_SURFACE_WIDTH - width || y > MAX_SURFACE_HEIGHT - height) {
		return false;
	}

	const int neededWidth  = std::max(canvas.width(), x + width);
	const int neededHeight = std::max(canvas.height(), y + height);

	if (neededWidth == canvas.width() && neededHeight == canvas.height()) {
		return true;
	}

	// Copied into a larger canvas rather than reallocated blank, so tiles already received survive a
	// resize. A sender whose resolution changes otherwise flashes back to black.
	QImage grown(neededWidth, neededHeight, QImage::Format_RGB32);
	grown.fill(Qt::black);

	if (!canvas.isNull()) {
		QPainter painter(&grown);
		painter.drawImage(0, 0, canvas);
	}

	canvas = grown;

	return true;
}

void VideoGrid::onVideoUnitReceived(unsigned int senderSession, unsigned int streamID, unsigned int x, unsigned int y,
									const QByteArray &encodedTile) {
	QImage tile;

	// Explicitly JPEG rather than letting Qt sniff the format. Sniffing would let a sender pick the
	// decoder by crafting a header, which turns every image format Qt supports into attack surface.
	if (!tile.loadFromData(encodedTile, "JPEG") || tile.isNull()) {
		return;
	}

	const bool isNewSender = !m_surfaces.contains(senderSession);

	if (isNewSender && senderCount() >= MAX_SENDERS) {
		return;
	}

	Surface &surface = m_surfaces[senderSession];

	// A new stream from the same sender means new dimensions or a new source, so the old picture is no
	// longer part of the same image.
	if (!isNewSender && surface.streamID != streamID) {
		surface.canvas = QImage();
	}

	surface.streamID = streamID;

	if (!growToFit(surface.canvas, static_cast< int >(x), static_cast< int >(y), tile.width(), tile.height())) {
		if (isNewSender) {
			m_surfaces.remove(senderSession);
		}

		return;
	}

	{
		QPainter painter(&surface.canvas);
		painter.drawImage(static_cast< int >(x), static_cast< int >(y), tile);
	}

	if (isNewSender) {
		emit senderCountChanged(senderCount());
	}

	update();
}

void VideoGrid::removeSender(unsigned int senderSession) {
	if (m_surfaces.remove(senderSession) > 0) {
		emit senderCountChanged(senderCount());
		update();
	}
}

void VideoGrid::clear() {
	if (m_surfaces.isEmpty()) {
		return;
	}

	m_surfaces.clear();

	emit senderCountChanged(0);
	update();
}

void VideoGrid::paintEvent(QPaintEvent *) {
	QPainter painter(this);
	painter.fillRect(rect(), Qt::black);

	if (m_surfaces.isEmpty()) {
		return;
	}

	// A roughly square arrangement, which is what every other video call looks like and needs no layout
	// configuration to be reasonable at any participant count.
	const int count   = senderCount();
	const int columns = static_cast< int >(std::ceil(std::sqrt(static_cast< double >(count))));
	const int rows    = (count + columns - 1) / columns;

	const int cellWidth  = width() / columns;
	const int cellHeight = height() / rows;

	if (cellWidth <= 0 || cellHeight <= 0) {
		return;
	}

	int index = 0;

	for (auto it = m_surfaces.constBegin(); it != m_surfaces.constEnd(); ++it, ++index) {
		if (it->canvas.isNull()) {
			continue;
		}

		const int column = index % columns;
		const int row    = index / columns;

		const QRect cell(column * cellWidth, row * cellHeight, cellWidth, cellHeight);

		// Scaled to fit inside the cell without distorting it. Letterboxing is the honest presentation:
		// stretching someone's screen share to fill a cell makes text unreadable.
		const QSize scaled = it->canvas.size().scaled(cell.size(), Qt::KeepAspectRatio);

		const QRect target(cell.x() + (cell.width() - scaled.width()) / 2,
						   cell.y() + (cell.height() - scaled.height()) / 2, scaled.width(), scaled.height());

		painter.drawImage(target, it->canvas);
	}
}
