// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "CameraVideoSource.h"

#include <QtMultimedia/QCamera>
#include <QtMultimedia/QCameraFormat>
#include <QtMultimedia/QMediaCaptureSession>
#include <QtMultimedia/QMediaDevices>
#include <QtMultimedia/QVideoFrame>
#include <QtMultimedia/QVideoFrameFormat>
#include <QtMultimedia/QVideoSink>

#include <tuple>

namespace {

/**
 * Picks a capture format that does not make us decode JPEG per frame.
 *
 * Left to itself the backend picks the device's preferred format, which on most UVC webcams is MJPEG at
 * the largest resolution the device offers. Every such frame then goes through libjpeg inside
 * QVideoFrame::toImage(), and a short buffer there produces the "Corrupt JPEG data: premature end of
 * data segment" / "JPEG data does not contain EOI marker" pairs seen at capture time. An uncompressed
 * format has no decoder in the path at all, and is cheaper besides.
 *
 * Uncompressed modes are the ones a camera tends to offer at a low frame rate in its larger sizes, so
 * this ranks a usable rate above raw size, and does not go above 720p: the frames are scaled down for
 * display and encoding anyway, and an uncompressed 1080p stream is 3 MB per frame off the bus for no
 * visible gain.
 *
 * Returns a null format when the device only offers JPEG (or offers nothing), in which case the caller
 * leaves the choice to the backend and relies on the JPEG validation in handleFrame().
 */
QCameraFormat pickUncompressedFormat(const QCameraDevice &device) {
	constexpr float minFrameRate = 15.0f;
	constexpr qint64 maxArea     = 1280 * 720;

	// Ranked worst to best, so a larger tuple is a better format. A format that clears both the rate and
	// the size limit beats one that clears only the rate, which beats one that clears neither. Within a
	// tier the biggest frame that fits under the limit wins, but among frames that are over it the
	// smallest does - hence the negated area, which reverses the comparison for exactly that case.
	const auto rank = [](const QCameraFormat &format) {
		const auto area = static_cast< qint64 >(format.resolution().width()) * format.resolution().height();
		const int tier  = (format.maxFrameRate() >= minFrameRate ? 2 : 0) + (area <= maxArea ? 1 : 0);

		return std::make_tuple(tier, area <= maxArea ? area : -area, format.maxFrameRate());
	};

	QCameraFormat best;

	for (const QCameraFormat &format : device.videoFormats()) {
		if (format.pixelFormat() == QVideoFrameFormat::Format_Jpeg
			|| format.pixelFormat() == QVideoFrameFormat::Format_Invalid) {
			continue;
		}

		if (best.isNull() || rank(format) > rank(best)) {
			best = format;
		}
	}

	return best;
}

/**
 * Decodes an MJPEG frame, rejecting truncated buffers instead of handing them to libjpeg.
 *
 * Cameras do sometimes deliver a short buffer, and the decoder's response is a warning per frame plus a
 * half-drawn image. A JPEG ends in the EOI marker FF D9, so a buffer whose last meaningful bytes are not
 * that one is incomplete and worth dropping. Trailing padding is skipped because drivers commonly zero-
 * fill the rest of a fixed-size buffer after the image data.
 *
 * Returns a null QImage for a frame that cannot be mapped or is incomplete.
 */
QImage decodeJpegFrame(const QVideoFrame &frame) {
	QVideoFrame mapped(frame);

	if (!mapped.map(QVideoFrame::ReadOnly)) {
		return QImage();
	}

	const uchar *data = mapped.bits(0);
	qsizetype size    = mapped.mappedBytes(0);

	while (size > 0 && data && data[size - 1] == 0x00) {
		--size;
	}

	QImage image;

	if (data && size >= 2 && data[size - 2] == 0xFF && data[size - 1] == 0xD9) {
		image.loadFromData(data, static_cast< int >(size), "JPEG");
	}

	mapped.unmap();

	return image;
}

} // namespace

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

	const QCameraFormat format = pickUncompressedFormat(m_device);

	if (!format.isNull()) {
		m_camera->setCameraFormat(format);
	}

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

	// toImage() on a JPEG frame passes the buffer straight to libjpeg, which warns once per frame on a
	// truncated one. decodeJpegFrame() drops those instead.
	const QImage image =
		frame.pixelFormat() == QVideoFrameFormat::Format_Jpeg ? decodeJpegFrame(frame) : frame.toImage();

	if (image.isNull()) {
		// A format QImage cannot represent. Dropping the frame is correct; warning per frame would not
		// be, since this would repeat at the full frame rate.
		return;
	}

	emit frameReady(image, static_cast< std::uint64_t >(m_clock.elapsed().count()));
}
