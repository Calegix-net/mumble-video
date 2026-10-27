// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "StreamVolumeSlider.h"

#include <QtGui/QMouseEvent>
#include <QtWidgets/QStyle>
#include <QtWidgets/QStyleOptionSlider>

StreamVolumeSlider::StreamVolumeSlider(QWidget *parent) : QSlider(Qt::Horizontal, parent) {
	// The pointer is the only way to move this - the bar it lives on is revealed by hover and takes no
	// keyboard focus - so say so, rather than leaving an arrow cursor that suggests nothing happens here.
	setCursor(Qt::PointingHandCursor);

	// Tracking is what makes the picture follow the drag instead of jumping when the button comes up.
	// It is QSlider's default; set anyway, because everything else about this class depends on it.
	setTracking(true);
}

int StreamVolumeSlider::valueForPosition(int x, const QRect &groove, int handleWidth, int minimum, int maximum,
										 bool upsideDown) {
	const int span     = groove.width() - handleWidth;
	const int position = x - groove.x() - handleWidth / 2;

	// A groove narrower than its own handle has no span to interpolate over, and sliderValueFromPosition()
	// divides by it. It should not happen - the styles here give a 12px handle a full-width groove - but a
	// theme is free to disagree, and a division by zero is not the way to find out.
	if (span <= 0) {
		return position > 0 ? maximum : minimum;
	}

	return QStyle::sliderValueFromPosition(minimum, maximum, position, span, upsideDown);
}

int StreamVolumeSlider::valueAt(int x) const {
	QStyleOptionSlider option;
	initStyleOption(&option);

	const QRect groove = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
	const QRect handle = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);

	return valueForPosition(x, groove, handle.width(), minimum(), maximum(), option.upsideDown);
}

void StreamVolumeSlider::mousePressEvent(QMouseEvent *event) {
	if (event->button() != Qt::LeftButton) {
		QSlider::mousePressEvent(event);
		return;
	}

	// setSliderDown() before the position, so that the one valueChanged this produces is already seen as
	// part of a drag by anything watching isSliderDown() - the grid keeps the hover bar up while it is.
	setSliderDown(true);
	setSliderPosition(valueAt(static_cast< int >(event->position().x())));

	event->accept();
}

void StreamVolumeSlider::mouseMoveEvent(QMouseEvent *event) {
	if (!isSliderDown()) {
		QSlider::mouseMoveEvent(event);
		return;
	}

	// No check that the pointer is still inside the widget: it deliberately is not required to be. Qt
	// grabs the mouse for the press, so a drag that wanders off the tile - off the window, even - keeps
	// arriving here, and setSliderPosition() clamps it to the ends.
	setSliderPosition(valueAt(static_cast< int >(event->position().x())));

	event->accept();
}

void StreamVolumeSlider::mouseReleaseEvent(QMouseEvent *event) {
	if (event->button() == Qt::LeftButton && isSliderDown()) {
		setSliderDown(false);
		event->accept();
		return;
	}

	QSlider::mouseReleaseEvent(event);
}
