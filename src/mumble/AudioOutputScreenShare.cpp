// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "AudioOutputScreenShare.h"

#include <opus.h>
#include <speex/speex_resampler.h>

#include <algorithm>
#include <cstring>

namespace {

// The encoder side (ScreenAudioBroadcaster) always produces 48kHz stereo.
constexpr unsigned int SOURCE_SAMPLE_RATE = 48000;
constexpr unsigned int CHANNELS           = 2;

// Opus's largest legal frame at 48kHz is 120ms. Sized to that worst case so a single opus_decode_float
// call can never overflow the stack buffer it writes into, regardless of what a peer sends.
constexpr int MAX_DECODED_FRAMES = 5760;

} // namespace

AudioOutputScreenShare::AudioOutputScreenShare(unsigned int mixerFreq) : m_mixerFreq(mixerFreq) {
	bStereo     = true;
	iBufferSize = 0;

	int decoderError = 0;
	m_opusState       = opus_decoder_create(static_cast< opus_int32 >(SOURCE_SAMPLE_RATE),
											static_cast< int >(CHANNELS), &decoderError);

	if (decoderError != OPUS_OK) {
		m_opusState = nullptr;
	}

	if (m_mixerFreq != 0 && m_mixerFreq != SOURCE_SAMPLE_RATE) {
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

void AudioOutputScreenShare::addOpusPacket(const QByteArray &opusPacket) {
	if (!m_opusState) {
		return;
	}

	float decoded[MAX_DECODED_FRAMES * CHANNELS];

	const int decodedFrames =
		opus_decode_float(m_opusState, reinterpret_cast< const unsigned char * >(opusPacket.constData()),
						  static_cast< opus_int32 >(opusPacket.size()), decoded, MAX_DECODED_FRAMES, 0);

	if (decodedFrames <= 0) {
		return;
	}

	QMutexLocker lock(&m_mutex);

	if (m_resampler) {
		// Sized generously; speex_resampler_process_interleaved_float reports back how much it actually
		// produced, same discipline ScreenAudioBroadcaster's own resampling step uses on the send side.
		std::vector< float > resampled(
			(static_cast< std::size_t >(decodedFrames) * m_mixerFreq / SOURCE_SAMPLE_RATE + 32) * CHANNELS);

		spx_uint32_t inLen  = static_cast< spx_uint32_t >(decodedFrames);
		spx_uint32_t outLen = static_cast< spx_uint32_t >(resampled.size() / CHANNELS);

		speex_resampler_process_interleaved_float(m_resampler, decoded, &inLen, resampled.data(), &outLen);

		m_ring.insert(m_ring.end(), resampled.begin(),
					 resampled.begin() + static_cast< std::ptrdiff_t >(outLen) * CHANNELS);
	} else {
		m_ring.insert(m_ring.end(), decoded, decoded + static_cast< std::size_t >(decodedFrames) * CHANNELS);
	}

	// Bounded so a sustained decode-faster-than-playback situation cannot grow this without limit. Half a
	// second of stereo audio is generous slack for ordinary jitter while still bounding memory; this
	// should not normally trigger at all, since both sides are paced in real time.
	const std::size_t maxRingSamples = (static_cast< std::size_t >(m_mixerFreq ? m_mixerFreq : SOURCE_SAMPLE_RATE) / 2)
									  * CHANNELS;

	if (m_ring.size() > maxRingSamples) {
		m_ring.erase(m_ring.begin(), m_ring.begin() + static_cast< std::ptrdiff_t >(m_ring.size() - maxRingSamples));
	}
}

bool AudioOutputScreenShare::prepareSampleBuffer(unsigned int frameCount) {
	const unsigned int sampleCount = frameCount * CHANNELS;

	resizeBuffer(sampleCount);

	QMutexLocker lock(&m_mutex);

	const std::size_t available = m_ring.size();
	const std::size_t toCopy    = std::min(static_cast< std::size_t >(sampleCount), available);

	if (toCopy > 0) {
		std::memcpy(pfBuffer, m_ring.data(), toCopy * sizeof(float));
	}

	if (toCopy < sampleCount) {
		// An underrun - the network is behind, or nothing has arrived yet. Silence rather than stale or
		// uninitialised data, and rather than stalling the mixer waiting for more.
		std::memset(pfBuffer + toCopy, 0, static_cast< std::size_t >(sampleCount - toCopy) * sizeof(float));
	}

	m_ring.erase(m_ring.begin(), m_ring.begin() + static_cast< std::ptrdiff_t >(toCopy));

	return true;
}
