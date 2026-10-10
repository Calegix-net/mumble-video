// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "AudioOutputScreenShare.h"

#include <QtCore/QDateTime>

#include <opus.h>
#include <speex/speex_resampler.h>

#include <algorithm>
#include <cstring>

namespace {

// The encoder side (ScreenAudioBroadcaster) always produces 48kHz stereo.
constexpr unsigned int SOURCE_SAMPLE_RATE = 48000;
constexpr unsigned int CHANNELS           = 2;

// Opus's largest legal frame at 48kHz is 120ms. Sized to that worst case so a single opus_decode_float
// call can never overflow the buffer it writes into, regardless of what a peer sends.
constexpr int MAX_DECODED_FRAMES = 5760;

} // namespace

AudioOutputScreenShare::AudioOutputScreenShare(unsigned int mixerFreq)
	: m_mixerFreq(mixerFreq ? mixerFreq : SOURCE_SAMPLE_RATE) {
	bStereo     = true;
	iBufferSize = 0;

	m_statsEnabled = !qEnvironmentVariableIsEmpty("MUMBLE_VIDEO_STATS");

	int decoderError = 0;
	m_opusState =
		opus_decoder_create(static_cast< opus_int32 >(SOURCE_SAMPLE_RATE), static_cast< int >(CHANNELS), &decoderError);

	if (decoderError != OPUS_OK) {
		m_opusState = nullptr;
	}

	if (m_mixerFreq != SOURCE_SAMPLE_RATE) {
		int resamplerError = 0;
		m_resampler = speex_resampler_init(CHANNELS, SOURCE_SAMPLE_RATE, m_mixerFreq, 3, &resamplerError);

		if (resamplerError != RESAMPLER_ERR_SUCCESS) {
			m_resampler = nullptr;
		}
	}
}

AudioOutputScreenShare::~AudioOutputScreenShare() {
	if (m_resampler) {
		speex_resampler_destroy(m_resampler);
	}

	if (m_opusState) {
		opus_decoder_destroy(m_opusState);
	}
}

void AudioOutputScreenShare::addOpusPacket(std::uint64_t sequence, const QByteArray &opusPacket) {
	if (!m_opusState || opusPacket.isEmpty()) {
		return;
	}

	++m_statsPackets;

	if (m_haveSequence && sequence < m_nextSequence) {
		// Its slot has already been played or concealed: decoding it now would play it out of order and
		// desynchronise the decoder's state.
		++m_statsLate;
		maybeLogStats();

		return;
	}

	if (m_haveSequence && sequence < m_highestSequence) {
		++m_statsReordered;
	}
	m_highestSequence = std::max(m_highestSequence, sequence);

	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	m_reorder.emplace(sequence, HeldPacket{ opusPacket, now });

	drainReorderBuffer(now);
	maybeLogStats();
}

void AudioOutputScreenShare::drainReorderBuffer(qint64 nowMsec) {
	while (!m_reorder.empty()) {
		const auto first = m_reorder.begin();

		if (!m_haveSequence || first->first == m_nextSequence) {
			decodeInOrder(first->first, first->second.data, 0);
			m_reorder.erase(first);
			continue;
		}

		// A gap before the oldest held packet. Give the missing ones a little time - they are usually
		// merely overtaken, not lost - but not so long, or so many packets deep, that the queue the mixer
		// plays from runs dry waiting.
		const bool waitedLongEnough =
			nowMsec - first->second.arrivalMsec >= REORDER_WAIT_MSEC || m_reorder.size() >= MAX_REORDER_PACKETS;
		if (!waitedLongEnough) {
			return;
		}

		decodeInOrder(first->first, first->second.data, first->first - m_nextSequence);
		m_reorder.erase(first);
	}
}

void AudioOutputScreenShare::decodeInOrder(std::uint64_t sequence, const QByteArray &opusPacket,
										   std::uint64_t missingBefore) {
	const auto *data       = reinterpret_cast< const unsigned char * >(opusPacket.constData());
	const opus_int32 bytes = static_cast< opus_int32 >(opusPacket.size());
	float decoded[MAX_DECODED_FRAMES * CHANNELS];

	if (missingBefore > 0 && missingBefore <= MAX_CONCEALED_PACKETS) {
		// Everything but the last missing packet can only be guessed (PLC). The last one immediately
		// precedes this packet, whose in-band FEC carries a low-bitrate copy of it.
		for (std::uint64_t i = 0; i + 1 < missingBefore; ++i) {
			const int concealed = opus_decode_float(m_opusState, nullptr, 0, decoded, m_lastFrameSize, 0);
			if (concealed > 0) {
				queueDecoded(decoded, concealed);
				++m_statsConcealed;
			}
		}

		const int recovered = opus_decode_float(m_opusState, data, bytes, decoded, m_lastFrameSize, 1);
		if (recovered > 0) {
			queueDecoded(decoded, recovered);
			++m_statsFecRecovered;
		}
	}
	// A longer gap - the sender paused, or a long outage - is left as silence: concealing it would stretch
	// a guess across a stretch of time the listener would hear as a drone.

	const int decodedFrames = opus_decode_float(m_opusState, data, bytes, decoded, MAX_DECODED_FRAMES, 0);

	if (decodedFrames > 0) {
		m_lastFrameSize = decodedFrames;
		queueDecoded(decoded, decodedFrames);
	}

	m_haveSequence = true;
	m_nextSequence = sequence + 1;
}

void AudioOutputScreenShare::queueDecoded(const float *frames, int frameCount) {
	std::vector< float > resampled;
	const float *source      = frames;
	std::size_t sourceFrames = static_cast< std::size_t >(frameCount);

	if (m_resampler) {
		// Sized generously; the resampler reports back how much it actually produced.
		resampled.resize((static_cast< std::size_t >(frameCount) * m_mixerFreq / SOURCE_SAMPLE_RATE + 32) * CHANNELS);

		spx_uint32_t inLen  = static_cast< spx_uint32_t >(frameCount);
		spx_uint32_t outLen = static_cast< spx_uint32_t >(resampled.size() / CHANNELS);

		speex_resampler_process_interleaved_float(m_resampler, frames, &inLen, resampled.data(), &outLen);

		source       = resampled.data();
		sourceFrames = outLen;
	}

	QMutexLocker lock(&m_mutex);

	// Compact only once the consumed prefix is both large and the bigger part of the vector, so the mixer
	// thread's reads stay a pointer bump and this memmove stays rare.
	if (m_readPos > 16384 && m_readPos * 2 > m_queue.size()) {
		m_queue.erase(m_queue.begin(), m_queue.begin() + static_cast< std::ptrdiff_t >(m_readPos));
		m_readPos = 0;
	}

	m_queue.insert(m_queue.end(), source, source + sourceFrames * CHANNELS);

	const std::size_t queuedFrames = (m_queue.size() - m_readPos) / CHANNELS;
	const std::size_t maxFrames    = static_cast< std::size_t >(m_mixerFreq) * MAX_BUFFER_MSEC / 1000;

	if (queuedFrames > maxFrames) {
		// A long stall delivered a backlog all at once. Catching up at the drift rate would hold the delay
		// for many seconds; dropping back to the prebuffer level is one audible jump instead.
		const std::size_t keepFrames = static_cast< std::size_t >(m_mixerFreq) * PREBUFFER_MSEC / 1000;
		m_readPos += (queuedFrames - keepFrames) * CHANNELS;
		m_statsTrimmed.fetch_add(1, std::memory_order_relaxed);
	}
}

bool AudioOutputScreenShare::prepareSampleBuffer(unsigned int frameCount) {
	const unsigned int sampleCount = frameCount * CHANNELS;

	resizeBuffer(sampleCount);

	QMutexLocker lock(&m_mutex);

	const std::size_t available = (m_queue.size() - m_readPos) / CHANNELS;
	const std::size_t prebuffer = static_cast< std::size_t >(m_mixerFreq) * PREBUFFER_MSEC / 1000;
	const std::size_t highWater = static_cast< std::size_t >(m_mixerFreq) * HIGH_WATER_MSEC / 1000;

	if (!m_playing) {
		if (available < prebuffer) {
			std::memset(pfBuffer, 0, static_cast< std::size_t >(sampleCount) * sizeof(float));

			return true;
		}

		m_playing = true;
	}

	const bool catchUp    = available > highWater;
	const float *source   = m_queue.data() + m_readPos;
	std::size_t consumed  = 0;
	std::size_t produced  = 0;
	std::uint64_t skipped = 0;

	while (produced < frameCount && consumed < available) {
		if (catchUp && --m_skipCountdown == 0) {
			m_skipCountdown = DRIFT_SKIP_INTERVAL;

			if (consumed + 1 < available) {
				++consumed;
				++skipped;
			}
		}

		pfBuffer[produced * CHANNELS]     = source[consumed * CHANNELS];
		pfBuffer[produced * CHANNELS + 1] = source[consumed * CHANNELS + 1];
		++produced;
		++consumed;
	}

	if (produced < frameCount) {
		// Ran dry. Silence for the rest, and rebuild the cushion before playing again rather than
		// stuttering through every packet as it trickles in.
		std::memset(pfBuffer + produced * CHANNELS, 0, (frameCount - produced) * CHANNELS * sizeof(float));
		m_playing = false;
		m_statsUnderruns.fetch_add(1, std::memory_order_relaxed);
	}

	m_readPos += consumed * CHANNELS;

	if (skipped) {
		m_statsDriftSkipped.fetch_add(skipped, std::memory_order_relaxed);
	}

	return true;
}

void AudioOutputScreenShare::maybeLogStats() {
	if (!m_statsEnabled) {
		return;
	}

	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	if (m_statsLastLogMsec == 0) {
		m_statsLastLogMsec = now;
		return;
	}
	if (now - m_statsLastLogMsec < 2000) {
		return;
	}
	m_statsLastLogMsec = now;

	std::size_t queuedFrames = 0;
	{
		QMutexLocker lock(&m_mutex);
		queuedFrames = (m_queue.size() - m_readPos) / CHANNELS;
	}

	qInfo("ScreenAudioStats packets=%llu reordered=%llu concealed=%llu fecRecovered=%llu late=%llu underruns=%llu "
		  "driftSkipped=%llu trimmed=%llu bufferedMs=%llu",
		  static_cast< unsigned long long >(m_statsPackets), static_cast< unsigned long long >(m_statsReordered),
		  static_cast< unsigned long long >(m_statsConcealed), static_cast< unsigned long long >(m_statsFecRecovered),
		  static_cast< unsigned long long >(m_statsLate),
		  static_cast< unsigned long long >(m_statsUnderruns.exchange(0)),
		  static_cast< unsigned long long >(m_statsDriftSkipped.exchange(0)),
		  static_cast< unsigned long long >(m_statsTrimmed.exchange(0)),
		  static_cast< unsigned long long >(queuedFrames * 1000 / m_mixerFreq));

	m_statsPackets = m_statsReordered = m_statsConcealed = m_statsFecRecovered = m_statsLate = 0;
}
