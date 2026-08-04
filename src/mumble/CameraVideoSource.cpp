// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "CameraVideoSource.h"

#include <QtMultimedia/QCamera>
#include <QtMultimedia/QMediaCaptureSession>
#include <QtMultimedia/QMediaDevices>
#include <QtMultimedia/QVideoFrame>
#include <QtMultimedia/QVideoSink>

QList< QCameraDevice > CameraVideoSource::availableCameras() {
	return QMediaDevices::videoInputs();
}

QCameraDevice CameraVideoSource::defaultCamera() {
	return QMediaDevices::defaultVideoInput();
}

CameraVideoSource::CameraVideoSource(const QCameraDevice &device, QObject *parent)
	: VideoSource(parent), m_device(device) {
}

CameraVideoSource::~CameraVideoSource() {
	// Ordering matters: the sink outlives the connection only if the camera is stopped first, otherwise
	// a frame can arrive while members are being torn down.
	CameraVideoSource::stop();
}

bool CameraVideoSource::isRunning() const {
	return m_camera && m_camera->isActive();
}

QString CameraVideoSource::describe() const {
	if (m_device.isNull()) {
		return QStringLiteral("No camera");
	}

	return m_device.description();
}

bool CameraVideoSource::start() {
	if (isRunning()) {
		return true;
	}

	if (m_device.isNull()) {
		emit failed(tr("No camera is available"));

		return false;
	}

	m_camera  = std::make_unique< QCamera >(m_device);
	m_session = std::make_unique< QMediaCaptureSession >();
	m_sink    = std::make_unique< QVideoSink >();

	QObject::connect(m_sink.get(), &QVideoSink::videoFrameChanged, this,
					 [this](const QVideoFrame &frame) { handleFrame(frame); });

	// Qt reports camera problems asynchronously: a camera that is present but busy, or one the user
	// denied at the OS permission prompt, fails here rather than at start().
	//
	// The signal is delivered from inside the backend's own call stack - on V4L2, from within
	// QV4L2Camera::setActive - and every handler of failed() responds by tearing the source down, which
	// destroys this QCamera. Freeing an object while Qt is still executing a method on it crashes when
	// that call returns. Two clients on one machine reach this every time, because V4L2 grants exclusive
	// access and the second open always fails.
	//
	// So the report is handed to the event loop and emitted once the backend has finished unwinding.
	QObject::connect(m_camera.get(), &QCamera::errorOccurred, this,
					 [this](QCamera::Error error, const QString &errorString) {
						 if (error == QCamera::NoError) {
							 return;
						 }

						 QMetaObject::invokeMethod(
							 this, [this, errorString]() { emit failed(errorString); }, Qt::QueuedConnection);
					 });

	m_session->setCamera(m_camera.get());
	m_session->setVideoSink(m_sink.get());

	m_clock.restart();
	m_camera->start();

	// start() is asynchronous on every backend, so isActive() being false here is normal and not a
	// failure. Genuine failures arrive through errorOccurred above.
	return true;
}

void CameraVideoSource::stop() {
	if (m_camera) {
		m_camera->stop();
	}

	// Torn down in the reverse of construction so no frame can be delivered into a half-destroyed
	// object.
	m_session.reset();
	m_sink.reset();
	m_camera.reset();
}

void CameraVideoSource::handleFrame(const QVideoFrame &frame) {
	if (!frame.isValid()) {
		return;
	}

	const QImage image = frame.toImage();

	if (image.isNull()) {
		// A format QImage cannot represent. Dropping the frame is correct; warning per frame would not
		// be, since this would repeat at the full frame rate.
		return;
	}

	emit frameReady(image, static_cast< std::uint64_t >(m_clock.elapsed().count()));
}
