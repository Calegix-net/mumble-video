// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "VideoBandwidthProbe.h"

#include "VideoEncoder.h"
#include "VideoSource.h"

#include <algorithm>
#include <utility>

VideoBandwidthProbe::VideoBandwidthProbe(QObject *parent) : QObject(parent) {
	m_ticker.setInterval(100);
	connect(&m_ticker, &QTimer::timeout, this, &VideoBandwidthProbe::onTick);
}

VideoBandwidthProbe::~VideoBandwidthProbe() {
	VideoBandwidthProbe::stop();
}

int VideoBandwidthProbe::progress() const {
	if (m_candidates.empty()) {
		return 100;
	}

	const qint64 done = static_cast< qint64 >(m_current) * m_measureMs
						+ (m_running ? std::min< qint64 >(m_roundTimer.elapsed(), m_measureMs) : 0);

	return static_cast< int >(done * 100 / (static_cast< qint64 >(m_candidates.size()) * m_measureMs));
}

bool VideoBandwidthProbe::start(std::unique_ptr< VideoSource > source, const QSize &resolution, unsigned int framerate,
								const std::vector< unsigned int > &candidates, int measureMs) {
	stop();

	if (!source || candidates.empty()) {
		return false;
	}

	m_resolution = resolution;
	m_framerate  = std::max(1u, framerate);
	m_measureMs  = std::max(200, measureMs);
	m_candidates = candidates;
	m_results.clear();
	m_current     = 0;
	m_framesTotal = 0;

	connect(source.get(), &VideoSource::frameReady, this, &VideoBandwidthProbe::onFrameReady);

	if (!source->start()) {
		return false;
	}

	m_source  = std::move(source);
	m_running = true;

	beginRound();
	m_ticker.start();

	return true;
}

void VideoBandwidthProbe::stop() {
	m_ticker.stop();
	m_running = false;

	if (m_source) {
		m_source->stop();
		m_source.reset();
	}
}

void VideoBandwidthProbe::beginRound() {
	m_encoder.setBitrate(m_candidates[m_current]);
	m_encoder.setFramerate(m_framerate);
	m_encoder.reset();

	m_bytesThisRound = 0;
	m_roundTimer.restart();
}

void VideoBandwidthProbe::onFrameReady(const QImage &frame, std::uint64_t captureTimestampUsec) {
	if (!m_running || frame.isNull()) {
		return;
	}

	const QImage scaled =
		m_resolution.isValid() ? frame.scaled(m_resolution, Qt::KeepAspectRatio, Qt::SmoothTransformation) : frame;

	for (const EncodedVideoUnit &unit : m_encoder.encode(scaled, 0, m_framesTotal, captureTimestampUsec)) {
		m_bytesThisRound += unit.payload.size();
	}

	m_framesTotal++;
}

void VideoBandwidthProbe::finishRound() {
	const qint64 elapsed = std::max< qint64 >(1, m_roundTimer.elapsed());

	// Divided by wall-clock rather than by the nominal frame rate. A camera that cannot keep up delivers
	// fewer frames and therefore genuinely costs less, and reporting the nominal figure would overstate
	// what the user is actually about to send.
	const double kbits = static_cast< double >(m_bytesThisRound) * 8.0 / static_cast< double >(elapsed);

	m_results.push_back(kbits);

	emit candidateMeasured(m_candidates[m_current], kbits);

	m_current++;

	if (m_current >= m_candidates.size()) {
		stop();

		emit finished();

		return;
	}

	beginRound();
}

void VideoBandwidthProbe::onTick() {
	if (!m_running) {
		return;
	}

	if (m_roundTimer.elapsed() >= m_measureMs) {
		finishRound();
	}
}
