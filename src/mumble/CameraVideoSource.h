// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_CAMERAVIDEOSOURCE_H_
#define MUMBLE_MUMBLE_CAMERAVIDEOSOURCE_H_

#include "Timer.h"
#include "VideoSource.h"

#include <QtCore/QList>
#include <QtMultimedia/QCameraDevice>

#include <memory>

class QCamera;
class QMediaCaptureSession;
class QVideoSink;
class QVideoFrame;

/**
 * Captures from a camera using Qt Multimedia.
 *
 * Qt Multimedia is used rather than a per-platform backend because it already wraps V4L2, AVFoundation
 * and Media Foundation, and it ships with the Qt the client already depends on. That keeps webcam
 * support from dragging in a new third-party library on three platforms, which for a project that links
 * statically on Windows and macOS is the difference between a feature and a packaging project.
 *
 * Frames are converted to QImage here so that nothing downstream depends on Qt Multimedia. That
 * conversion is not free, and it is the obvious first thing to optimise if capture ever shows up in a
 * profile, but correctness and portability come first: QVideoFrame can arrive in any of a dozen pixel
 * formats depending on the camera and platform, and QImage conversion is the only path that handles all
 * of them.
 */
class CameraVideoSource : public VideoSource {
	Q_OBJECT

public:
	/**
	 * Cameras the platform is willing to tell us about. May be empty: no camera, no permission, or a
	 * headless machine. Callers must handle that rather than assuming index 0 exists.
	 */
	static QList< QCameraDevice > availableCameras();

	/**
	 * The platform's preferred camera, or a default-constructed (null) device if there is none.
	 */
	static QCameraDevice defaultCamera();

	explicit CameraVideoSource(const QCameraDevice &device, QObject *parent = nullptr);
	~CameraVideoSource() override;

	bool start() override;
	void stop() override;
	bool isRunning() const override;
	QString describe() const override;

protected:
	void handleFrame(const QVideoFrame &frame);

	QCameraDevice m_device;

	std::unique_ptr< QCamera > m_camera;
	std::unique_ptr< QMediaCaptureSession > m_session;
	std::unique_ptr< QVideoSink > m_sink;

	// Capture timestamps are taken from a monotonic clock started when the source starts, not from the
	// wall clock, and are only comparable within one stream.
	Timer m_clock;
};

#endif // MUMBLE_MUMBLE_CAMERAVIDEOSOURCE_H_
