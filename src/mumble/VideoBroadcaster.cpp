// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "VideoBroadcaster.h"

#include "VideoSource.h"

#include <QPointer>
#include <QtCore/QMetaObject>
#include <QtCore/QThread>
#include <algorithm>
#include <utility>

// ---- VideoEncodeCore -------------------------------------------------------------------------------------

bool VideoEncodeCore::isScreenCodec() const {
	return m_codec == VideoBroadcaster::CODEC_VP9 || m_codec == VideoBroadcaster::CODEC_H264;
}

void VideoEncodeCore::setCodec(int codec) {
	m_codec = codec;
	m_vpx.setContent(codec == VideoBroadcaster::CODEC_VP9 ? VpxContent::Screen : VpxContent::Camera);
}

void VideoEncodeCore::configure(int codec, unsigned int bitrateKbps, unsigned int framerate, int tileQuality,
								int tileSize) {
	setCodec(codec);
	m_framerate = std::max(1u, framerate);

	m_vpx.setBitrate(bitrateKbps);
	m_vpx.setFramerate(framerate);

#ifdef USE_H264
	m_h264.setBitrate(bitrateKbps);
	m_h264.setFramerate(framerate);
	// Four seconds of frames. Viewers ask for a keyframe themselves after a loss or on joining, so this
	// only bounds the worst case; and a keyframe squeezed into a few frames' budget is a visible dip in
	// quality on a moving picture, which every two seconds reads as pulsing.
	m_h264.setKeyframeInterval(m_framerate * 4);
#endif

	m_encoder.setQuality(tileQuality);
	m_encoder.setTileSize(tileSize);
}

void VideoEncodeCore::reset(bool reopenHardware) {
	m_encoder.reset();
	m_vpx.reset();
#ifdef USE_H264
	// Only on a new stream: closing and reopening the hardware encoder costs a hundred milliseconds or more
	// on some drivers, for what a forced keyframe already asks of the open one.
	if (reopenHardware) {
		m_h264.reset();
	}
#else
	Q_UNUSED(reopenHardware);
#endif
	m_lastEncoded         = QImage();
	m_unchangedFrames     = 0;
	m_lastEncodeTimestamp = 0;
}

VideoEncodeCore::Result VideoEncodeCore::process(const Job &job) {
	Result result;
	result.captureTimestampUsec = job.captureTimestampUsec;
	result.keyframeRequested    = job.forceKeyframe;

	// Receivers refuse frames beyond their surface bound (VideoGrid::MAX_SURFACE_WIDTH/HEIGHT), so a frame
	// larger than that must never reach the encoder - a dual-monitor screen share at 5120x1440 would
	// otherwise be encoded and sent only to be discarded. Scaled here, once, on the sending side.
	const QImage frame = job.frame.size() == job.targetSize
							 ? job.frame
							 : job.frame.scaled(job.targetSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

	// A screen that has stopped changing. Under constant bitrate the encoder keeps spending its whole budget
	// on an unchanged picture, refining it frame after frame - measured for VP9 at about 10 KB a frame, the
	// full 3 Mbit/s, on a still 1080p screen; a hardware H.264 encoder does the same - and some capture
	// backends (PipeWire, the mock screen) keep delivering frames while nothing changes. So an unchanged
	// picture is encoded for one second, which is plenty to sharpen it, and then not again until it changes,
	// except for a keep-alive every two seconds. QImage compares the pixel data directly: a few milliseconds
	// at most for the largest frame, and on this thread rather than the GUI's when encoding is threaded.
	if (isScreenCodec()) {
		const bool unchanged = !m_lastEncoded.isNull() && m_lastEncoded == frame;
		m_unchangedFrames    = unchanged ? m_unchangedFrames + 1 : 0;

		const bool keepAliveDue = job.captureTimestampUsec >= m_lastEncodeTimestamp
								  && job.captureTimestampUsec - m_lastEncodeTimestamp >= 2000000;

		if (!job.forceKeyframe && !keepAliveDue && m_unchangedFrames > m_framerate) {
			return result;
		}
	}

	QElapsedTimer timer;
	timer.start();

#ifdef USE_H264
	if (m_codec == VideoBroadcaster::CODEC_H264) {
		result.units = m_h264.encode(frame, job.streamID, m_frameNumber, job.captureTimestampUsec, job.forceKeyframe);
	} else
#endif
		result.units =
			m_codec == VideoBroadcaster::CODEC_TILED
				? m_encoder.encode(frame, job.streamID, m_frameNumber, job.captureTimestampUsec, job.forceKeyframe)
				: m_vpx.encode(frame, job.streamID, m_frameNumber, job.captureTimestampUsec, job.forceKeyframe);

	result.encodeUsec = static_cast< std::uint64_t >(timer.nsecsElapsed() / 1000);
	result.encoded    = true;

	// libvpx (VP8, VP9) returns its output for a frame immediately, so nothing back means the frame was
	// dropped - too large for one transport unit - and the next one is forced to be a keyframe. Its number
	// is not used up: a viewer would take the gap for a lost frame and wait for it. The tiled codec's empty
	// output means "nothing changed", and a hardware H.264 encoder may hand a frame back a call later under
	// the number it was given, so both keep counting.
	if (!result.units.empty() || m_codec == VideoBroadcaster::CODEC_TILED || m_codec == VideoBroadcaster::CODEC_H264) {
		m_frameNumber++;
	}
	m_lastEncoded = frame;
	if (!result.units.empty()) {
		m_lastEncodeTimestamp = job.captureTimestampUsec;
	}

	return result;
}

// ---- VideoBroadcaster ------------------------------------------------------------------------------------

VideoBroadcaster::VideoBroadcaster(QObject *parent) : QObject(parent), m_core(std::make_shared< VideoEncodeCore >()) {
	m_stats.enabled = !qEnvironmentVariableIsEmpty("MUMBLE_VIDEO_STATS");
}

VideoBroadcaster::~VideoBroadcaster() {
	VideoBroadcaster::stop();

	if (m_encodeThread) {
		m_encodeThread->quit();
		m_encodeThread->wait();
	}
}

void VideoBroadcaster::setThreadedEncoding(bool threaded) {
	if (threaded == (m_encodeThread != nullptr)) {
		return;
	}

	if (!threaded) {
		m_encodeThread->quit();
		m_encodeThread->wait();
		delete m_encodeThread;
		m_encodeThread = nullptr;
		m_encodeWorker = nullptr;
		return;
	}

	m_encodeThread = new QThread(this);
	m_encodeThread->setObjectName(QStringLiteral("VideoEncode"));
	// An object living on the thread, for queued calls to run there. Deleted with the thread's event loop.
	m_encodeWorker = new QObject();
	m_encodeWorker->moveToThread(m_encodeThread);
	connect(m_encodeThread, &QThread::finished, m_encodeWorker, &QObject::deleteLater);
	m_encodeThread->start();
}

void VideoBroadcaster::withCore(std::function< void(VideoEncodeCore &) > fn) {
	if (!m_encodeWorker) {
		fn(*m_core);
		return;
	}

	// Queued on the encode thread, so it runs in order with the jobs already sent there.
	const std::shared_ptr< VideoEncodeCore > core = m_core;
	QMetaObject::invokeMethod(m_encodeWorker, [core, fn = std::move(fn)]() { fn(*core); }, Qt::QueuedConnection);
}

void VideoBroadcaster::noteEncodeStats(bool keyframe, std::size_t units, std::uint64_t bytes,
									   std::uint64_t encodeUsec) {
	if (!m_stats.window.isValid()) {
		m_stats.window.start();
	}

	++m_stats.frames;
	m_stats.keyframes += keyframe ? 1 : 0;
	m_stats.units += units;
	m_stats.bytes += bytes;
	m_stats.encodeUsec += encodeUsec;
	m_stats.maxEncodeUsec    = std::max(m_stats.maxEncodeUsec, encodeUsec);
	m_stats.maxUnitsPerFrame = std::max< std::uint64_t >(m_stats.maxUnitsPerFrame, units);

	const qint64 elapsedMsec = m_stats.window.elapsed();
	if (elapsedMsec < 2000) {
		return;
	}

	const double seconds = static_cast< double >(elapsedMsec) / 1000.0;
	qInfo("VideoStats tx stream=%u codec=%d fps=%.1f keyframes=%llu units/s=%.0f kbit/s=%.0f encodeMsAvg=%.2f "
		  "encodeMsMax=%.2f maxUnitsPerFrame=%llu dropped=%llu threaded=%d",
		  m_streamID, m_codec, static_cast< double >(m_stats.frames) / seconds,
		  static_cast< unsigned long long >(m_stats.keyframes), static_cast< double >(m_stats.units) / seconds,
		  static_cast< double >(m_stats.bytes) * 8.0 / 1000.0 / seconds,
		  static_cast< double >(m_stats.encodeUsec) / 1000.0 / static_cast< double >(m_stats.frames),
		  static_cast< double >(m_stats.maxEncodeUsec) / 1000.0,
		  static_cast< unsigned long long >(m_stats.maxUnitsPerFrame),
		  static_cast< unsigned long long >(m_stats.dropped), m_encodeThread ? 1 : 0);

	const bool enabled = m_stats.enabled;
	m_stats            = EncodeStats();
	m_stats.enabled    = enabled;
	m_stats.window.start();
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
	connect(m_source.get(), &VideoSource::captureIdle, this, &VideoBroadcaster::onCaptureIdle);
	// Queued: the handler destroys the source, and every capture backend emits failed() from inside
	// its own call stack - portal callbacks, PipeWire trampolines, camera backends. Tearing the sender
	// down mid-emit is the use-after-free the camera error path once shipped.
	const QPointer< VideoSource > startedSource(m_source.get());
	connect(
		m_source.get(), &VideoSource::failed, this,
		[this, startedSource](const QString &reason) {
			if (!startedSource || m_source.get() != startedSource.data())
				return;
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
	++m_generation;

	// The first frame of a stream has to be complete: a new receiver has nothing to build on.
	withCore([](VideoEncodeCore &core) { core.reset(true); });
	m_forceKeyframe = true;

	emit activeChanged(true);

	return true;
}

void VideoBroadcaster::stop() {
	m_lastFrame = QImage();
	m_lastTargetSize = QSize();
	m_captureActivity.invalidate();
	m_captureTimestamp    = 0;
	m_lastEncodeTimestamp = 0;
	m_pendingJob.reset();
	++m_generation;
	if (!m_source) {
		return;
	}

	m_source->stop();
	m_source.reset();

	emit activeChanged(false);
}

void VideoBroadcaster::requestKeyframe() {
	withCore([](VideoEncodeCore &core) { core.reset(false); });
	m_forceKeyframe = true;
	// A new viewer also needs a picture when capture is healthy but unchanged. Do not
	// perpetually replay a cached frame from a source that has stopped reporting activity.
	if (m_source && m_source->isRunning() && !m_lastFrame.isNull() && m_captureActivity.isValid()
		&& m_captureActivity.elapsed() < 2000) {
		encodeFrame(m_lastFrame, m_captureTimestamp);
	}
}

void VideoBroadcaster::onCaptureIdle(std::uint64_t captureTimestampUsec) {
	if (!m_source || !m_source->isRunning()) {
		return;
	}
	m_captureActivity.restart();
	m_captureTimestamp = captureTimestampUsec;
	// This is driven by successful backend polls, never by an unconditional replay timer.
	// Two seconds also keeps idle shares below the receiver's stall and server's expiry thresholds.
	if (!m_lastFrame.isNull() && captureTimestampUsec >= m_lastEncodeTimestamp
		&& captureTimestampUsec - m_lastEncodeTimestamp >= 2000000) {
		// A keyframe for the tiled codec, whose "nothing changed" is no output at all. The whole-frame screen
		// codecs instead encode the unchanged picture as an ordinary frame - a few bytes that keep the stream
		// alive - since a screen keyframe every two seconds of stillness would be a 50-120 KB burst for nothing.
		m_forceKeyframe = m_forceKeyframe || !isScreenCodec();
		encodeFrame(m_lastFrame, captureTimestampUsec);
	}
}

void VideoBroadcaster::setCodec(int codec) {
#ifdef USE_H264
	const bool h264 = codec == CODEC_H264;
#else
	const bool h264 = false;
#endif
	m_codec = (h264 || codec == CODEC_TILED || codec == CODEC_VP9) ? codec : CODEC_VP8;

	const int effective = m_codec;
	withCore([effective](VideoEncodeCore &core) { core.setCodec(effective); });
}

void VideoBroadcaster::configure(int codec, unsigned int bitrateKbps, unsigned int framerate, int tileQuality,
								 int tileSize) {
	setCodec(codec);

	const int effective = m_codec;
	withCore([effective, bitrateKbps, framerate, tileQuality, tileSize](VideoEncodeCore &core) {
		core.configure(effective, bitrateKbps, framerate, tileQuality, tileSize);
	});
}

void VideoBroadcaster::onFrameReady(const QImage &sourceFrame, std::uint64_t captureTimestampUsec) {
	if (!m_source || sourceFrame.isNull()) {
		return;
	}

	m_captureActivity.restart();
	m_captureTimestamp = captureTimestampUsec;
	encodeFrame(sourceFrame, captureTimestampUsec);
}

QSize VideoBroadcaster::targetSizeFor(const QSize &source) const {
	const int maxWidth  = m_codec == CODEC_VP9 ? MAX_VP9_WIDTH : MAX_ENCODED_WIDTH;
	const int maxHeight = m_codec == CODEC_VP9 ? MAX_VP9_HEIGHT : MAX_ENCODED_HEIGHT;

	if (source.width() <= maxWidth && source.height() <= maxHeight) {
		return source;
	}

	// The size QImage::scaled() with Qt::KeepAspectRatio would produce, worked out without scaling.
	return source.scaled(maxWidth, maxHeight, Qt::KeepAspectRatio);
}

void VideoBroadcaster::encodeFrame(const QImage &frame, std::uint64_t captureTimestampUsec) {
	const QSize targetSize = targetSizeFor(frame.size());

	if (!m_lastTargetSize.isEmpty() && m_lastTargetSize != targetSize) {
		const auto previousID = m_streamID;
		m_streamID            = m_allocateStreamID ? m_allocateStreamID() : m_streamID + 1;
		// The H.264 encoder reopens itself at the new size on the next frame.
		withCore([](VideoEncodeCore &core) { core.reset(false); });
		m_forceKeyframe = true;
		// Set the cache first: a direct listener may immediately ask for a keyframe.
		m_lastFrame      = frame;
		m_lastTargetSize = targetSize;
		emit streamResized(previousID, m_streamID, targetSize);
	} else {
		m_lastFrame      = frame;
		m_lastTargetSize = targetSize;
	}

	// Low-rate sources may deliver unchanged frames rather than idle notifications. Not for the screen
	// codecs, which produce output for every frame they are given; see onCaptureIdle().
	if (!isScreenCodec() && captureTimestampUsec >= m_lastEncodeTimestamp
		&& captureTimestampUsec - m_lastEncodeTimestamp >= 2000000) {
		m_forceKeyframe = true;
	}

	emit previewFrame(frame);

	VideoEncodeCore::Job job;
	job.frame                = frame;
	job.targetSize           = targetSize;
	job.streamID             = m_streamID;
	job.captureTimestampUsec = captureTimestampUsec;
	job.forceKeyframe        = m_forceKeyframe;
	m_forceKeyframe          = false;

	submit(std::move(job));
}

void VideoBroadcaster::submit(VideoEncodeCore::Job job) {
	if (!m_encodeWorker) {
		deliver(m_core->process(job), m_generation);
		return;
	}

	if (m_encodeBusy) {
		// The encoder is still on an earlier frame. Keep only the newest one waiting - a keyframe request
		// carried by a frame being replaced is carried on - so a slow encode drops frames instead of adding
		// delay.
		if (m_pendingJob) {
			job.forceKeyframe = job.forceKeyframe || m_pendingJob->forceKeyframe;
			++m_stats.dropped;
		}
		m_pendingJob = std::move(job);
		return;
	}

	dispatch(std::move(job));
}

void VideoBroadcaster::dispatch(VideoEncodeCore::Job job) {
	m_encodeBusy = true;

	const std::shared_ptr< VideoEncodeCore > core = m_core;
	const std::uint64_t generation                = m_generation;
	const QPointer< VideoBroadcaster > self(this);

	QMetaObject::invokeMethod(
		m_encodeWorker,
		[core, job = std::move(job), generation, self]() {
			VideoEncodeCore::Result result = core->process(job);

			QMetaObject::invokeMethod(
				self.data(),
				[self, result = std::move(result), generation]() {
					if (!self) {
						return;
					}
					self->m_encodeBusy = false;
					self->deliver(result, generation);

					if (self->m_pendingJob) {
						VideoEncodeCore::Job next = std::move(*self->m_pendingJob);
						self->m_pendingJob.reset();
						self->dispatch(std::move(next));
					}
				},
				Qt::QueuedConnection);
		},
		Qt::QueuedConnection);
}

void VideoBroadcaster::deliver(const VideoEncodeCore::Result &result, std::uint64_t generation) {
	if (generation != m_generation || !result.encoded) {
		return;
	}

	if (m_stats.enabled) {
		std::uint64_t bytes = 0;
		for (const EncodedVideoUnit &unit : result.units) {
			bytes += unit.payload.size();
		}
		noteEncodeStats(result.keyframeRequested, result.units.size(), bytes, result.encodeUsec);
	}

	if (!result.units.empty()) {
		m_lastEncodeTimestamp = result.captureTimestampUsec;
	}

	// An empty result is normal and means the picture has not changed, so there is nothing a receiver
	// needs in order to stay correct.
	for (const EncodedVideoUnit &unit : result.units) {
		emit unitReady(unit.header, QByteArray(reinterpret_cast< const char * >(unit.payload.data()),
											   static_cast< int >(unit.payload.size())));
	}
}
