// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_AUDIOOUTPUTSCREENSHARE_H_
#define MUMBLE_MUMBLE_AUDIOOUTPUTSCREENSHARE_H_

#include "AudioOutputBuffer.h"

#include <QtCore/QByteArray>
#include <QtCore/QMutex>

#include <vector>

struct OpusDecoder;
struct SpeexResamplerState_;
typedef struct SpeexResamplerState_ SpeexResamplerState;

/**
 * Plays back a screen share's accompanying audio.
 *
 * Modelled on AudioOutputSample rather than AudioOutputSpeech: this is not voice, has no talk-detection
 * or per-sender codec-switch handling, and must not be spatially attenuated by the sharer's avatar
 * position - fPos is left at its base-class default of {0,0,0}, exactly the non-positional treatment
 * AudioOutput::playSample's own UI sounds already get.
 *
 * Unlike AudioOutputSpeech, the packets arriving here came over the video transport
 * (VideoStreamDispatcher::opusUnitReceived), not Mumble::Protocol::AudioData - there is no speex jitter
 * buffer, sequence-number loss concealment, or talking indicator here. A short ring buffer is enough:
 * VideoFragmentation's own reassembly already smooths ordinary network jitter before a packet reaches
 * addOpusPacket().
 *
 * addOpusPacket() decodes immediately and is expected to be called from the main thread, in response to
 * VideoStreamDispatcher's queued opusUnitReceived signal - decoding there rather than inside
 * prepareSampleBuffer() matters because the latter runs on the real-time mixing path, where doing any
 * more work than copying already-decoded samples out of the ring buffer risks an audible glitch.
 */
class AudioOutputScreenShare : public AudioOutputBuffer {
private:
	Q_OBJECT
	Q_DISABLE_COPY(AudioOutputScreenShare)

public:
	explicit AudioOutputScreenShare(unsigned int mixerFreq);
	~AudioOutputScreenShare() override;

	/// Decodes one Opus packet and appends the result to the ring buffer prepareSampleBuffer() reads
	/// from. Safe to call from any thread - the ring buffer is mutex-protected - but expected to run on
	/// the main thread in practice, per the class comment above.
	void addOpusPacket(const QByteArray &opusPacket);

	/// Always reports itself alive: unlike a sound file, there is no natural end this class can detect on
	/// its own. The stream's actual end arrives out-of-band, as a VideoState announcing active=false, and
	/// is handled by removing this buffer from AudioOutput directly rather than through this return value.
	bool prepareSampleBuffer(unsigned int frameCount) override;

protected:
	OpusDecoder *m_opusState         = nullptr;
	SpeexResamplerState *m_resampler = nullptr;
	unsigned int m_mixerFreq         = 0;

	QMutex m_mutex;

	/// Interleaved stereo float samples at m_mixerFreq, awaiting playback. A FIFO: consumed from the
	/// front by prepareSampleBuffer(), appended to at the back by addOpusPacket().
	std::vector< float > m_ring;
};

#endif // MUMBLE_MUMBLE_AUDIOOUTPUTSCREENSHARE_H_
