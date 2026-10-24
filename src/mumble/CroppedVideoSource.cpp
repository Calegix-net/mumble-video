// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "CroppedVideoSource.h"

#include <QtCore/QObject>
#include <QtCore/QtGlobal>

#include <algorithm>
#include <cmath>
#include <utility>

CroppedVideoSource::CroppedVideoSource(std::unique_ptr< VideoSource > inner, const QRect &region, QObject *parent)
	: VideoSource(parent), m_inner(std::move(inner)), m_region(region) {
	if (!m_inner) {
		return;
	}

	connect(m_inner.get(), &VideoSource::frameReady, this, &CroppedVideoSource::onInnerFrame);

	// Passed straight through: an idle poll found no changed pixels whether or not we are cropping, and
	// a backend that failed has failed for the region too.
	connect(m_inner.get(), &VideoSource::captureIdle, this, &VideoSource::captureIdle);
	connect(m_inner.get(), &VideoSource::failed, this, &VideoSource::failed);
}

CroppedVideoSource::~CroppedVideoSource() = default;

bool CroppedVideoSource::start() {
	return m_inner && m_inner->start();
}

void CroppedVideoSource::stop() {
	if (m_inner) {
		m_inner->stop();
	}
}

bool CroppedVideoSource::isRunning() const {
	return m_inner && m_inner->isRunning();
}

QString CroppedVideoSource::describe() const {
	if (!m_inner) {
		return QObject::tr("Region");
	}

	//: %1 is what is being captured (a display), %2 and %3 the size of the region taken from it.
	return QObject::tr("%1 (region %2x%3)").arg(m_inner->describe()).arg(m_region.width()).arg(m_region.height());
}

QRect CroppedVideoSource::rectFromDragPoints(const QPoint &first, const QPoint &second) {
	const int left   = std::min(first.x(), second.x());
	const int top    = std::min(first.y(), second.y());
	const int right  = std::max(first.x(), second.x());
	const int bottom = std::max(first.y(), second.y());

	// QRect(topLeft, bottomRight) treats bottomRight as inclusive, so dragging from 10 to 310 selects
	// pixels 10 through 310 - the same 301 columns the rubber band covered.
	return QRect(QPoint(left, top), QPoint(right, bottom));
}

QRect CroppedVideoSource::toCapturePixels(const QRect &logicalSelection, qreal devicePixelRatio) {
	if (logicalSelection.isNull() || devicePixelRatio <= 0.0) {
		return QRect();
	}

	// Normalised first: a selection dragged right-to-left or bottom-to-top has a negative width or
	// height until it is, and every comparison below assumes left <= right.
	const QRect selection = logicalSelection.normalized();

	// Converted as edges rather than as an origin plus a size. Rounding a size independently of its
	// origin lets the same drag produce a different width depending on where it started, and leaves a
	// gap or an overlap against anything measured from the neighbouring edge.
	const int left   = static_cast< int >(std::lround(selection.left() * devicePixelRatio));
	const int top    = static_cast< int >(std::lround(selection.top() * devicePixelRatio));
	const int right  = static_cast< int >(std::lround((selection.left() + selection.width()) * devicePixelRatio));
	const int bottom = static_cast< int >(std::lround((selection.top() + selection.height()) * devicePixelRatio));

	return QRect(QPoint(left, top), QSize(right - left, bottom - top));
}

QRect CroppedVideoSource::clampToFrame(const QRect &region, const QSize &frameSize) {
	if (region.isNull() || frameSize.isEmpty()) {
		return QRect();
	}

	const QRect frame   = QRect(QPoint(0, 0), frameSize);
	const QRect visible = region.normalized().intersected(frame);

	if (visible.isEmpty()) {
		return QRect();
	}

	// & ~1 rather than a rounding function, to match roundDownToEven() in VpxCodec.cpp exactly: both
	// dimensions have to be even for 4:2:0, and agreeing with the encoder here is what stops every frame
	// being rescaled on its way through.
	const int width  = visible.width() & ~1;
	const int height = visible.height() & ~1;

	if (width <= 0 || height <= 0) {
		return QRect();
	}

	return QRect(visible.topLeft(), QSize(width, height));
}

void CroppedVideoSource::onInnerFrame(const QImage &frame, std::uint64_t captureTimestampUsec) {
	const QRect crop = clampToFrame(m_region, frame.size());

	if (crop.isNull()) {
		// Nothing of the selection is on screen any more. Reported once and then silent: the alternative
		// - sending the whole frame - would share more than was selected, and sending nothing without
		// saying so looks like the share froze.
		if (!m_reportedRegionLost) {
			m_reportedRegionLost = true;

			emit failed(QObject::tr("The shared region is no longer within the captured screen."));
		}

		return;
	}

	m_reportedRegionLost = false;

	// copy() of a sub-rectangle, not a reference into the frame: the crop is handed to the encoder on
	// another thread, and QImage's implicit sharing would otherwise keep the whole uncropped frame alive
	// behind it - which for a 4K display is most of the memory the crop was supposed to save.
	emit frameReady(frame.copy(crop), captureTimestampUsec);
}
