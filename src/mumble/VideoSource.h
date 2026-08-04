// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_VIDEOSOURCE_H_
#define MUMBLE_MUMBLE_VIDEOSOURCE_H_

#include "Timer.h"

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtGui/QImage>

#include <cstdint>

/**
 * Something that produces video frames.
 *
 * Capture is abstracted for two reasons. The obvious one is that the platform backends differ. The
 * more important one is that a camera is hardware: no continuous integration machine has one, and
 * neither does a container. Without a source that can be driven deterministically, everything
 * downstream of capture - tiling, encoding, fragmentation, reassembly - could only ever be tested
 * against hand-built inputs that drift away from what the real pipeline produces.
 *
 * Frames are delivered as QImage rather than QVideoFrame so that nothing below this interface depends
 * on Qt Multimedia.
 */
class VideoSource : public QObject {
	Q_OBJECT

public:
	explicit VideoSource(QObject *parent = nullptr) : QObject(parent) {}
	~VideoSource() override = default;

	/**
	 * Begins producing frames. Returns whether the source started; a camera that is missing, busy or
	 * refused by the platform's permission prompt fails here rather than silently producing nothing.
	 */
	virtual bool start() = 0;

	virtual void stop() = 0;

	virtual bool isRunning() const = 0;

	/**
	 * Human-readable description of what is being captured, for the UI and for logs.
	 */
	virtual QString describe() const = 0;

signals:
	/**
	 * @param frame The captured image. Always a valid, non-null image.
	 * @param captureTimestampUsec Microseconds on a monotonic clock, taken as close to capture as the
	 *   backend allows. This is the only timing anchor the protocol carries.
	 */
	void frameReady(const QImage &frame, std::uint64_t captureTimestampUsec);

	/**
	 * Emitted when capture stops for a reason other than a stop() call: the device was unplugged, the
	 * permission was revoked, or the backend failed.
	 */
	void failed(const QString &reason);
};

/**
 * A VideoSource that generates frames in software.
 *
 * Used by the tests, and useful by hand when diagnosing the pipeline without involving a camera. By
 * default it produces frames only when pumped, so a test controls exactly how many frames exist and
 * what their timestamps are; call setInterval() to have it free-run on a timer instead.
 */
class SyntheticVideoSource : public VideoSource {
	Q_OBJECT

public:
	SyntheticVideoSource(int width, int height, QObject *parent = nullptr);

	bool start() override;
	void stop() override;
	bool isRunning() const override { return m_running; }
	QString describe() const override;

	/**
	 * Emits exactly one frame, whose content depends on the frame index so that successive frames
	 * differ. Returns the image emitted.
	 *
	 * @param captureTimestampUsec Timestamp to report for this frame.
	 */
	QImage pump(std::uint64_t captureTimestampUsec);

	/**
	 * Renders the frame that pump() would emit next, without emitting it.
	 */
	QImage render(std::uint64_t frameIndex) const;

	/**
	 * How much of the frame changes between consecutive frames, as a percentage of area. 100 models a
	 * camera, where sensor noise makes every tile differ. A small value models screen content, where
	 * most of the frame is static between frames.
	 */
	void setChangeRatio(int percent) { m_changePercent = percent; }

	/**
	 * Emits a frame every `milliseconds` once started, instead of only when pumped.
	 *
	 * Pull mode is what a test wants when it needs to control exactly how many frames exist. Anything
	 * driven by a real capture callback -- the bandwidth probe, for one -- needs frames to arrive on
	 * their own, and would otherwise sit measuring an entirely still picture.
	 */
	void setInterval(int milliseconds);

protected:
	int m_width;
	int m_height;
	int m_changePercent  = 100;
	bool m_running       = false;
	std::uint64_t m_next = 0;

	int m_intervalMs = 0;
	QTimer m_timer;
	Timer m_clock;
};

#endif // MUMBLE_MUMBLE_VIDEOSOURCE_H_
