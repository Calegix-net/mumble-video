// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "RegionSelectOverlay.h"

#include "CroppedVideoSource.h"

#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QScreen>

#include <algorithm>

RegionSelectOverlay::RegionSelectOverlay(QScreen *screen, QWidget *parent)
	: QDialog(parent, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool), m_screen(screen) {
	setWindowTitle(tr("Select a region to share"));

	setAttribute(Qt::WA_DeleteOnClose, false);
	setCursor(Qt::CrossCursor);
	setMouseTracking(true);

	if (m_screen) {
		// Grabbed before the window is shown, so the overlay does not photograph itself.
		m_background = m_screen->grabWindow(0);

		// Positioned before being shown: a window placed after the fact can land on whichever display the
		// window manager felt like, which on a multi-monitor desktop means dimming the wrong one.
		setGeometry(m_screen->geometry());
	}
}

QRect RegionSelectOverlay::selectionRect() const {
	if (!m_haveDrag) {
		return QRect();
	}

	return CroppedVideoSource::rectFromDragPoints(m_origin, m_current);
}

QRect RegionSelectOverlay::selectOn(QScreen *screen, QWidget *parent) {
	if (!screen) {
		return QRect();
	}

	RegionSelectOverlay overlay(screen, parent);

	// showFullScreen() rather than show(): a frameless window the size of the screen is still an ordinary
	// window, and a taskbar or dock would otherwise sit on top of the part of the screen it covers.
	overlay.showFullScreen();
	overlay.raise();
	overlay.activateWindow();

	if (overlay.exec() != QDialog::Accepted) {
		return QRect();
	}

	const QRect selection = overlay.selectionRect();

	if (selection.isEmpty()) {
		return QRect();
	}

	// The drag happened in device-independent pixels; the capture backend produces the display's real
	// ones. On a scaled display these differ, and the unconverted numbers describe the wrong part of the
	// screen entirely.
	return CroppedVideoSource::toCapturePixels(selection, screen->devicePixelRatio());
}

void RegionSelectOverlay::paintEvent(QPaintEvent *) {
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing, false);

	const QRect selection = selectionRect();

	// The screen as it was, then dimmed: what is about to be shared is the part that looks normal
	// rather than the part that looks highlighted.
	if (!m_background.isNull()) {
		painter.drawPixmap(rect(), m_background);
	} else {
		painter.fillRect(rect(), Qt::black);
	}

	painter.fillRect(rect(), QColor(0, 0, 0, 120));

	if (selection.isEmpty()) {
		painter.setPen(QColor(255, 255, 255, 200));
		painter.drawText(rect(), Qt::AlignCenter, tr("Drag to choose the area to share.\nPress Escape to cancel."));

		return;
	}

	// The selection redrawn from the grab at full brightness, so it stands out of the dimming as exactly
	// the pixels that will be sent.
	if (!m_background.isNull()) {
		const qreal scaleX = static_cast< qreal >(m_background.width()) / std::max(1, width());
		const qreal scaleY = static_cast< qreal >(m_background.height()) / std::max(1, height());
		const QRectF source(selection.x() * scaleX, selection.y() * scaleY, selection.width() * scaleX,
							selection.height() * scaleY);

		painter.drawPixmap(QRectF(selection), m_background, source);
	}

	painter.setPen(QPen(QColor(255, 255, 255, 220), 1));
	// adjusted() so the outline sits just outside the selection rather than over its edge pixels, which
	// are part of what gets shared.
	painter.drawRect(selection.adjusted(-1, -1, 0, 0));

	const QString label = tr("%1 x %2").arg(selection.width()).arg(selection.height());
	const QRect labelBox(selection.left(), selection.top() - 22, 120, 20);

	painter.setPen(QColor(255, 255, 255, 230));
	painter.drawText(labelBox.intersected(rect()).isEmpty() ? selection : labelBox, Qt::AlignLeft | Qt::AlignVCenter,
					 label);
}

void RegionSelectOverlay::mousePressEvent(QMouseEvent *event) {
	if (event->button() != Qt::LeftButton) {
		QDialog::mousePressEvent(event);

		return;
	}

	m_origin   = event->pos();
	m_current  = event->pos();
	m_dragging = true;
	m_haveDrag = true;

	update();
}

void RegionSelectOverlay::mouseMoveEvent(QMouseEvent *event) {
	if (!m_dragging) {
		return;
	}

	m_current = event->pos();

	update();
}

void RegionSelectOverlay::mouseReleaseEvent(QMouseEvent *event) {
	if (event->button() != Qt::LeftButton || !m_dragging) {
		QDialog::mouseReleaseEvent(event);

		return;
	}

	m_dragging = false;
	m_current  = event->pos();

	const QRect selection = selectionRect();

	// A click, or a flick of a few pixels, is not a selection. Reset rather than accept: sharing a
	// 3x2 rectangle because the mouse moved while clicking would be worse than making them drag again.
	if (selection.width() < MINIMUM_SELECTION || selection.height() < MINIMUM_SELECTION) {
		m_haveDrag = false;

		update();

		return;
	}

	accept();
}

void RegionSelectOverlay::keyPressEvent(QKeyEvent *event) {
	if (event->key() == Qt::Key_Escape) {
		reject();

		return;
	}

	QDialog::keyPressEvent(event);
}
