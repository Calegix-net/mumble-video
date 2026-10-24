// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_REGIONSELECTOVERLAY_H_
#define MUMBLE_MUMBLE_REGIONSELECTOVERLAY_H_

#include <QtCore/QPoint>
#include <QtCore/QRect>
#include <QtGui/QPixmap>
#include <QtWidgets/QDialog>

class QScreen;

/**
 * The "drag a box on your screen" step of a region share, as QuickTime's screen recorder does it: the
 * chosen display dims, the user drags a rectangle, and that rectangle is what gets shared.
 *
 * Covers one display rather than the whole desktop. The capture backend behind a region share duplicates
 * a single output, so a box spanning two monitors has no single frame to be cropped out of; keeping the
 * overlay to one screen makes that impossible to ask for rather than something to fail at afterwards.
 *
 * The rectangle it returns is in the display's own pixels, already converted from the device-independent
 * ones the drag happens in - see CroppedVideoSource::toCapturePixels().
 */
class RegionSelectOverlay : public QDialog {
	Q_OBJECT
public:
	/// Smallest selection, in device-independent pixels, that counts as a drag rather than a stray click.
	static constexpr int MINIMUM_SELECTION = 16;

	/**
	 * Dims @p screen and lets the user drag a rectangle on it.
	 *
	 * @returns The selection in @p screen's own pixels, relative to its top-left corner - ready to hand
	 *   to CroppedVideoSource. Null if the user pressed Escape, closed the overlay, or never dragged.
	 */
	static QRect selectOn(QScreen *screen, QWidget *parent = nullptr);

protected:
	void paintEvent(QPaintEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void keyPressEvent(QKeyEvent *event) override;

private:
	explicit RegionSelectOverlay(QScreen *screen, QWidget *parent);

	/// The selection in this widget's coordinates, which are device-independent.
	QRect selectionRect() const;

	QScreen *m_screen = nullptr;

	/// The screen as it looked when the overlay opened, painted as this window's own background.
	///
	/// A translucent window would have been the obvious way to dim the desktop, but on X11 that needs a
	/// compositing manager, and a session without one - i3, a bare XFCE, anything minimal - would have
	/// shown an invisible overlay with nothing to aim at. Painting a grab of the screen needs no
	/// compositor anywhere, and has the side benefit that the picture holds still while it is being
	/// chosen from.
	QPixmap m_background;

	QPoint m_origin;
	QPoint m_current;
	bool m_dragging = false;
	bool m_haveDrag = false;
};

#endif // MUMBLE_MUMBLE_REGIONSELECTOVERLAY_H_
