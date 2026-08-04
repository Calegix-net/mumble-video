// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_VIDEOBANDWIDTHPROBE_H_
#define MUMBLE_MUMBLE_VIDEOBANDWIDTHPROBE_H_

#include "VP8Codec.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QObject>
#include <QtCore/QSize>
#include <QtCore/QTimer>
#include <QtGui/QImage>

#include <cstdint>
#include <memory>
#include <vector>

class VideoSource;

/**
 * Measures what video actually costs, by encoding real frames at a series of candidate bitrates.
 *
 * This is the one part of video setup a user cannot reason about unaided: a bitrate number has no
 * observable meaning until a call goes badly. So rather than presenting a slider and hoping, the setup
 * flow encodes frames from the real camera at each candidate and reports what each genuinely produced.
 *
 * Deliberately separate from the wizard that presents it. Measurement is domain logic, and burying it in
 * a dialog would leave the only calculation anybody is asked to trust as the one piece that cannot be
 * tested without a camera and a window. It owns nothing but a capture source and an encoder, so a test
 * drives it with a synthetic source and no UI at all.
 */
class VideoBandwidthProbe : public QObject {
	Q_OBJECT

public:
	explicit VideoBandwidthProbe(QObject *parent = nullptr);
	~VideoBandwidthProbe() override;

	/// How long each candidate is measured for. Long enough for rate control to settle past the opening
	/// keyframe, short enough that a sweep is not tedious.
	static constexpr int DEFAULT_MEASURE_MS = 2500;

	/**
	 * Begins a sweep. The probe takes ownership of the source and stops it when finished.
	 *
	 * @param candidates Target bitrates in kbit/s, measured in the order given.
	 * @returns Whether the sweep started; it fails only if the source will not start.
	 */
	bool start(std::unique_ptr< VideoSource > source, const QSize &resolution, unsigned int framerate,
			   const std::vector< unsigned int > &candidates, int measureMs = DEFAULT_MEASURE_MS);

	void stop();

	bool isRunning() const { return m_running; }

	/// Measured rates in kbit/s, in candidate order. Complete only once finished() has fired.
	const std::vector< double > &results() const { return m_results; }

	const std::vector< unsigned int > &candidates() const { return m_candidates; }

	/// 0-100 across the whole sweep.
	int progress() const;

	/// The candidate currently being measured, or the total once finished.
	std::size_t currentCandidate() const { return m_current; }

signals:
	/// Fired as each candidate completes, so a caller can show progress meaningfully.
	void candidateMeasured(unsigned int targetKbps, double measuredKbps);

	void finished();

protected slots:
	void onFrameReady(const QImage &frame, std::uint64_t captureTimestampUsec);
	void onTick();

protected:
	std::unique_ptr< VideoSource > m_source;
	VP8Encoder m_encoder;

	QSize m_resolution;
	unsigned int m_framerate = 30;
	int m_measureMs          = DEFAULT_MEASURE_MS;

	std::vector< unsigned int > m_candidates;
	std::vector< double > m_results;

	std::size_t m_current        = 0;
	std::size_t m_bytesThisRound = 0;
	std::uint64_t m_framesTotal  = 0;

	QElapsedTimer m_roundTimer;
	QTimer m_ticker;
	bool m_running = false;

	void beginRound();
	void finishRound();
};

#endif // MUMBLE_MUMBLE_VIDEOBANDWIDTHPROBE_H_
