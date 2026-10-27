// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_STREAMVOLUMESLIDER_H_
#define MUMBLE_MUMBLE_STREAMVOLUMESLIDER_H_

#include "widgets/SemanticSlider.h"

#include <QtCore/QRect>

/**
 * The volume slider on a screen share - on its tile, and on the bar in fullscreen and picture-in-picture.
 *
 * A plain QSlider is the wrong feel for this. Qt's default is that pressing the groove *pages* towards the
 * cursor by pageStep and stops, so a press at the far end of a 110px track moved the level by 25% and let
 * go; getting to a particular level meant finding and grabbing a 12px knob, on a bar that is sitting on top
 * of moving video and is only there while the pointer is over the tile. People read that as the slider being
 * unresponsive, and they are right: nothing they did went where they aimed it.
 *
 * Here a press puts the level exactly where it was pressed and keeps dragging from there, which is what a
 * media player's volume slider does and what this one looked like it would do. The knob is then a hint about
 * the current level rather than the only thing worth hitting, so the whole width of the widget is the target
 * and no precision is needed in either direction.
 *
 * Only the left button does this. The middle and right buttons are left to QSlider, which ignores them, so a
 * right-click still reaches the tile underneath for its context menu.
 *
 * It derives from SemanticSlider so that a screen reader is told "150%" rather than "150" - the number on
 * its own means nothing without the scale, and this one goes to 200. setLevelText() keeps that in step
 * with the label beside it.
 */
class StreamVolumeSlider : public SemanticSlider {
	Q_OBJECT
	Q_DISABLE_COPY(StreamVolumeSlider)
public:
	explicit StreamVolumeSlider(QWidget *parent = nullptr);

	/// What a screen reader should read out for the current level, e.g. "150%". Set alongside the visible
	/// label, so the two cannot drift apart.
	void setLevelText(const QString &text);

	/**
	 * The value a press at @p x lands on, as a pure function of the geometry - the arithmetic of
	 * valueAt(), split out so it can be tested without a window system.
	 *
	 * The handle's own width is taken out of the usable span and half of it out of the position, so that
	 * the handle ends up centred under the cursor: without that, dragging to the right-hand end of the
	 * groove would stop short of the maximum by half a handle, and the level would never reach 200%.
	 *
	 * @param x Where the press landed, in the slider's own coordinates.
	 * @param groove The groove rectangle the style reports, in the same coordinates.
	 * @param handleWidth Width of the handle the style reports.
	 * @param minimum Lowest value, inclusive.
	 * @param maximum Highest value, inclusive.
	 * @param upsideDown Whether the style lays the values out right-to-left (QStyleOptionSlider::upsideDown).
	 * @return A value clamped to [minimum, maximum].
	 */
	static int valueForPosition(int x, const QRect &groove, int handleWidth, int minimum, int maximum,
								bool upsideDown);

protected:
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;

private:
	/// valueForPosition() fed with this widget's current style geometry.
	int valueAt(int x) const;
};

#endif
