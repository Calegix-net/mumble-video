// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "ScreenAudioBroadcaster.h"

#include "AudioLoopbackSource.h"

#include <opus.h>
#include <speex/speex_resampler.h>

#include <utility>

ScreenAudioBroadcaster::ScreenAudioBroadcaster(QObject *parent) : QObject(parent) {
}

ScreenAudioBroadcaster::~ScreenAudioBroadcaster() {
	ScreenAudioBroadcaster::stop();
}

QString ScreenAudioBroadcaster::describe() const {
	return m_source ? m_source->describe() : QString();
}

void ScreenAudioBroadcaster::configure(unsigned int bitrateKbps) {
	m_bitrateKbps = bitrateKbps;

	if (m_opusState) {
		opus_encoder_ctl(m_opusState, OPUS_SET_BITRATE(static_cast< opus_int32 >(m_bitrateKbps) * 1000));
	}
}

bool ScreenAudioBroadcaster::start(std::unique_ptr< AudioLoopbackSource > source, std::uint32_t streamID) {
	stop();

	if (!source) {
		return false;
	}

	m_source   = std::move(source);
	m_streamID = streamID;

	connect(m_source.get(), &AudioLoopbackSource::samplesReady, this, &ScreenAudioBroadcaster::onSamplesReady);
	// Queued for the same reason the video path is (see VideoBroadcaster): a backend may emit failed()
	// from inside its own call stack, and stop() here destroys the source - tearing it down mid-emit is
	// a use-after-free. The WASAPI sources already marshal failed() to this thread, but the queue keeps
	// this correct for any source that emits synchronously.
	connect(
		m_source.get(), &AudioLoopbackSource::failed, this,
		[this](const QString &reason) {
			stop();
			emit failed(reason);
		},
		Qt::QueuedConnection);

	int opusError = 0;
	m_opusState = opus_encoder_create(static_cast< opus_int32 >(TARGET_SAMPLE_RATE),
									  static_cast< int >(TARGET_CHANNELS), OPUS_APPLICATION_AUDIO, &opusError);

	if (opusError != OPUS_OK || !m_opusState) {
		m_opusState = nullptr;
		m_source.reset();
		emit failed(tr("Could not create an audio encoder"));

		return false;
	}

	opus_encoder_ctl(m_opusState, OPUS_SET_BITRATE(static_cast< opus_int32 >(m_bitrateKbps) * 1000));

	m_accumulator.clear();
	m_frameNumber = 0;
	m_loggedFirstSamples = false;
	m_loggedFirstUnit    = false;

	if (!m_source->start()) {
		opus_encoder_destroy(m_opusState);
		m_opusState = nullptr;
		m_source.reset();

		return false;
	}

	emit activeChanged(true);

	return true;
}

void ScreenAudioBroadcaster::stop() {
	if (m_resampler) {
		speex_resampler_destroy(m_resampler);
		m_resampler = nullptr;
	}

	m_resamplerInputRate    = 0;
	m_resamplerInputChannels = 0;
	m_accumulator.clear();

	if (m_opusState) {
		opus_encoder_destroy(m_opusState);
		m_opusState = nullptr;
	}

	if (!m_source) {
		return;
	}

	m_source->stop();
	m_source.reset();

	emit activeChanged(false);
}

void ScreenAudioBroadcaster::onSamplesReady(const QByteArray &pcm, std::uint64_t) {
	if (!m_source || !m_opusState) {
		return;
	}

	const unsigned int sourceChannels = m_source->channelCount();

	if (sourceChannels == 0 || m_source->sampleRate() == 0) {
		return;
	}

	const auto *floatData        = reinterpret_cast< const float * >(pcm.constData());
	const std::size_t totalFloats = static_cast< std::size_t >(pcm.size()) / sizeof(float);
	const std::size_t frameCount  = totalFloats / sourceChannels;

	if (frameCount == 0) {
		return;
	}

	if (!m_loggedFirstSamples) {
		m_loggedFirstSamples = true;

		qWarning("ScreenAudioBroadcaster: first samples from capture source (%u ch, %u Hz, %zu frames)",
				 sourceChannels, m_source->sampleRate(), frameCount);
	}

	appendResampledFrames(floatData, frameCount);
	encodeAccumulatedFrames();
}

void ScreenAudioBroadcaster::appendResampledFrames(const float *samples, std::size_t frameCount) {
	const unsigned int sourceChannels = m_source->channelCount();
	const unsigned int sourceRate     = m_source->sampleRate();

	// Channel-mixed to stereo first, at the source's own rate: fewer samples to carry through the
	// resampler when the source has more than two channels, and the resampler below only ever has to
	// handle exactly TARGET_CHANNELS regardless of what the device actually delivers.
	std::vector< float > stereo(frameCount * TARGET_CHANNELS);

	for (std::size_t i = 0; i < frameCount; ++i) {
		if (sourceChannels == 1) {
			const float sample                  = samples[i];
			stereo[i * TARGET_CHANNELS]         = sample;
			stereo[i * TARGET_CHANNELS + 1]     = sample;
		} else {
			// Two or more channels: the first two are taken as left/right verbatim. Correct for the
			// ordinary stereo case; for genuine surround output this discards the rear/LFE channels
			// rather than guessing at a mixdown, since this class has no way to know the actual channel
			// layout with certainty.
			stereo[i * TARGET_CHANNELS]     = samples[i * sourceChannels];
			stereo[i * TARGET_CHANNELS + 1] = samples[i * sourceChannels + 1];
		}
	}

	if (sourceRate == TARGET_SAMPLE_RATE) {
		m_accumulator.insert(m_accumulator.end(), stereo.begin(), stereo.end());

		return;
	}

	if (!m_resampler || m_resamplerInputRate != sourceRate) {
		if (m_resampler) {
			speex_resampler_destroy(m_resampler);
			m_resampler = nullptr;
		}

		int err = 0;
		// Quality 3: comfortably good for system audio at modest CPU cost, the same quality level
		// AudioOutputSpeech uses for voice resampling.
		m_resampler = speex_resampler_init(TARGET_CHANNELS, sourceRate, TARGET_SAMPLE_RATE, 3, &err);

		if (!m_resampler) {
			return;
		}

		m_resamplerInputRate    = sourceRate;
		m_resamplerInputChannels = TARGET_CHANNELS;
	}

	// Sized generously rather than computed exactly - the resampler reports back how much it actually
	// produced, and this only has to be large enough to never truncate a reasonable upsample.
	std::vector< float > resampled(stereo.size() * 4 + 256);

	spx_uint32_t inLen  = static_cast< spx_uint32_t >(frameCount);
	spx_uint32_t outLen = static_cast< spx_uint32_t >(resampled.size() / TARGET_CHANNELS);

	speex_resampler_process_interleaved_float(m_resampler, stereo.data(), &inLen, resampled.data(), &outLen);

	resampled.resize(static_cast< std::size_t >(outLen) * TARGET_CHANNELS);

	m_accumulator.insert(m_accumulator.end(), resampled.begin(), resampled.end());
}

void ScreenAudioBroadcaster::encodeAccumulatedFrames() {
	// Opus's own worst-case output size for any valid input, per the libopus documentation - safe
	// regardless of frame size or bitrate.
	constexpr int MAX_OPUS_PACKET_BYTES = 4000;

	while (m_accumulator.size() >= static_cast< std::size_t >(OPUS_FRAME_SAMPLES) * TARGET_CHANNELS) {
		unsigned char encoded[MAX_OPUS_PACKET_BYTES];

		const int encodedBytes = opus_encode_float(m_opusState, m_accumulator.data(), OPUS_FRAME_SAMPLES, encoded,
												   MAX_OPUS_PACKET_BYTES);

		// The consumed samples are erased regardless of whether encoding succeeded: leaving them in place
		// after a failure would mean re-attempting the same bytes forever rather than moving on.
		m_accumulator.erase(m_accumulator.begin(),
							m_accumulator.begin()
								+ static_cast< std::ptrdiff_t >(OPUS_FRAME_SAMPLES) * TARGET_CHANNELS);

		if (encodedBytes <= 0) {
			continue;
		}

		if (!m_loggedFirstUnit) {
			m_loggedFirstUnit = true;

			qWarning("ScreenAudioBroadcaster: first Opus unit encoded (%d bytes, stream %u)", encodedBytes,
					 m_streamID);
		}

		Mumble::Protocol::VideoUnitHeader header;
		header.streamID    = m_streamID;
		header.frameNumber = m_frameNumber++;
		header.unitID      = 0;
		header.isKeyframe  = true;
		header.isFrameEnd  = true;
		// x/y/width/height are left at their default of 0: meaningless for audio, and ignored by
		// VideoStreamDispatcher on the receiving end.

		emit unitReady(header, QByteArray(reinterpret_cast< const char * >(encoded), encodedBytes));
	}
}
