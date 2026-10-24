// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_CROPPEDVIDEOSOURCE_H_
#define MUMBLE_MUMBLE_CROPPEDVIDEOSOURCE_H_

#include "VideoSource.h"

#include <QtCore/QRect>
#include <QtCore/QSize>

#include <memory>

/**
 * Shares one rectangle of another source's picture instead of all of it - the "region" share, where the
 * user drags a box on their screen and only that box is sent.
 *
 * Wrapping a source rather than teaching each backend to crop is what makes this work everywhere at
 * once: DXGI, X11 and PipeWire all hand their frames to the same frameReady(), so cropping above them
 * costs nothing per platform. It is also strictly cheaper than the share it crops - fewer pixels reach
 * the encoder than the display capture it wraps would have sent.
 *
 * The region is in the pixels the backend produces, not in the screen coordinates a window manager
 * reports: those differ by the display's scale factor, and getting that wrong silently shares the wrong
 * part of the screen. toCapturePixels() is the conversion, kept here and tested rather than written
 * inline in the UI.
 */
class CroppedVideoSource : public VideoSource {
	Q_OBJECT
public:
	/**
	 * @param inner The source to crop. Owned from here on, started and stopped with this one.
	 * @param region In inner's frame pixels, with the frame's top-left as the origin.
	 */
	CroppedVideoSource(std::unique_ptr< VideoSource > inner, const QRect &region, QObject *parent = nullptr);
	~CroppedVideoSource() override;

	bool start() override;
	void stop() override;
	bool isRunning() const override;
	QString describe() const override;

	/**
	 * Maps a selection made in a widget's coordinates onto the pixels the capture backend produces.
	 *
	 * A selection is drawn in Qt's device-independent pixels, while a capture backend hands back the
	 * display's real ones; on a scaled display the two differ by devicePixelRatio, so a box drawn around
	 * the right-hand half of a 150% display describes the middle of the captured frame unless it is
	 * converted. Edges are converted rather than the origin and size, so that a box's right edge lands
	 * where its neighbour's left edge would and rounding cannot open a seam between them.
	 */
	static QRect toCapturePixels(const QRect &logicalSelection, qreal devicePixelRatio);

	/**
	 * The part of @p region that a frame of @p frameSize actually contains, with both dimensions rounded
	 * down to even.
	 *
	 * Even because VP8 and VP9 are 4:2:0 and the encoder rounds odd dimensions down anyway - doing it
	 * here means the frames handed on already match what will be encoded, instead of every one of them
	 * being rescaled by a pixel on its way through VideoBroadcaster.
	 *
	 * Clamped, never widened: a region that no longer fits - the display's resolution dropped, or it was
	 * unplugged and replaced by a smaller one - shrinks to what is left, and returns null once nothing is
	 * left at all. Falling back to the whole frame would quietly share more of the screen than the user
	 * selected, which is the one failure mode this must not have.
	 */
	static QRect clampToFrame(const QRect &region, const QSize &frameSize);

	/**
	 * The rectangle between two corners of a drag, whichever order they were dragged in.
	 *
	 * QRect's own two-point constructor takes a top-left and an *inclusive* bottom-right, so handing it
	 * the corners the wrong way round and calling normalized() does not give the same rectangle back -
	 * it loses a pixel on each axis. Taking the minimum and maximum of each coordinate instead makes a
	 * drag symmetric, and is what the selection overlay uses so that the band drawn on screen and the
	 * pixels captured are the same rectangle.
	 */
	static QRect rectFromDragPoints(const QPoint &first, const QPoint &second);

	/// The region as given, in capture pixels. Not clamped - that depends on each frame.
	QRect region() const { return m_region; }

private:
	void onInnerFrame(const QImage &frame, std::uint64_t captureTimestampUsec);

	std::unique_ptr< VideoSource > m_inner;
	QRect m_region;

	/// So a region that has fallen off the screen reports once rather than on every frame.
	bool m_reportedRegionLost = false;
};

#endif // MUMBLE_MUMBLE_CROPPEDVIDEOSOURCE_H_
