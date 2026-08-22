// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "VideoBroadcaster.h"

#include "VideoSource.h"

#include <utility>

VideoBroadcaster::VideoBroadcaster(QObject *parent) : QObject(parent) {
}

VideoBroadcaster::~VideoBroadcaster() {
	VideoBroadcaster::stop();
}

QString VideoBroadcaster::describe() const {
	return m_source ? m_source->describe() : QString();
}

bool VideoBroadcaster::start(std::unique_ptr< VideoSource > source) {
	stop();

	if (!source) {
		return false;
	}

	m_source = std::move(source);

	connect(m_source.get(), &VideoSource::frameReady, this, &VideoBroadcaster::onFrameReady);
	// Queued: the handler destroys the source, and every capture backend emits failed() from inside
	// its own call stack - portal callbacks, PipeWire trampolines, camera backends. Tearing the sender
	// down mid-emit is the use-after-free the camera error path once shipped.
	connect(
		m_source.get(), &VideoSource::failed, this,
		[this](const QString &reason) {
			stop();
			emit failed(reason);
		},
		Qt::QueuedConnection);

	if (!m_source->start()) {
		m_source.reset();

		return false;
	}

	// A new stream each time, because the dimensions or the source itself may have changed and a
	// receiver must not keep painting into the previous picture.
	if (m_everStarted && !m_streamIDSeeded) {
		m_streamID++;
	}

	m_streamIDSeeded = false;

	m_everStarted = true;

	// The first frame of a stream has to be complete: a new receiver has nothing to build on.
	m_encoder.reset();
	m_vp8.reset();
	m_forceKeyframe = true;

	emit activeChanged(true);

	return true;
}

void VideoBroadcaster::stop() {
	if (!m_source) {
		return;
	}

	m_source->stop();
	m_source.reset();

	emit activeChanged(false);
}

void VideoBroadcaster::requestKeyframe() {
	m_encoder.reset();
	m_vp8.reset();
	m_forceKeyframe = true;
}

void VideoBroadcaster::setCodec(int codec) {
	m_codec = codec == 1 ? 1 : 0;
}

void VideoBroadcaster::configure(int codec, unsigned int bitrateKbps, unsigned int framerate, int tileQuality,
								 int tileSize) {
	setCodec(codec);

	m_vp8.setBitrate(bitrateKbps);
	m_vp8.setFramerate(framerate);

	m_encoder.setQuality(tileQuality);
	m_encoder.setTileSize(tileSize);
}

void VideoBroadcaster::onFrameReady(const QImage &sourceFrame, std::uint64_t captureTimestampUsec) {
	if (!m_source) {
		return;
	}

	// Receivers refuse tiles beyond their surface bound (VideoGrid::MAX_SURFACE_WIDTH/HEIGHT), so a
	// frame larger than that must never reach the encoder - a dual-monitor screen share at 5120x1440
	// would otherwise encode and transmit tiles the far side is contractually going to discard,
	// showing the viewer a silently cropped picture. Scaled here, once, on the sending side.
	QImage frame = sourceFrame;

	if (frame.width() > MAX_ENCODED_WIDTH || frame.height() > MAX_ENCODED_HEIGHT) {
		frame = frame.scaled(MAX_ENCODED_WIDTH, MAX_ENCODED_HEIGHT, Qt::KeepAspectRatio,
							 Qt::SmoothTransformation);
	}

	emit previewFrame(frame);

	const std::vector< EncodedVideoUnit > units =
		m_codec == 0 ? m_vp8.encode(frame, m_streamID, m_frameNumber, captureTimestampUsec, m_forceKeyframe)
					 : m_encoder.encode(frame, m_streamID, m_frameNumber, captureTimestampUsec, m_forceKeyframe);

	m_forceKeyframe = false;
	m_frameNumber++;

	// An empty result is normal and means the picture has not changed, so there is nothing a receiver
	// needs in order to stay correct.
	for (const EncodedVideoUnit &unit : units) {
		emit unitReady(unit.header, QByteArray(reinterpret_cast< const char * >(unit.payload.data()),
											   static_cast< int >(unit.payload.size())));
	}
}
