// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_AUDIOOUTPUTSCREENSHARE_H_
#define MUMBLE_MUMBLE_AUDIOOUTPUTSCREENSHARE_H_

#include "AudioOutputBuffer.h"

#include <QtCore/QByteArray>
#include <QtCore/QMutex>

#include <atomic>
#include <cstdint>
#include <map>
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
 * The packets arrive over the video transport (VideoStreamDispatcher::opusUnitReceived), not as
 * Mumble::Protocol::AudioData, so none of the voice path's jitter handling applies and this class carries
 * its own, sized for music and game sound rather than speech:
 *
 * - A jitter buffer. Playback starts only once PREBUFFER_MSEC is queued, and an underrun goes back to
 *   prebuffering instead of playing every late packet the instant it lands. Without it ordinary network
 *   and main-thread jitter - this is decoded on the GUI thread, which also paints video - is heard as
 *   constant crackle.
 * - Loss concealment. The transport's frame number is the Opus packet sequence, so a gap is visible: the
 *   last missing packet is rebuilt from the next one's in-band FEC (the sender enables it), any before it
 *   are Opus PLC, and a late or duplicate packet is dropped instead of being played out of order.
 * - Drift correction. The sender's and this machine's audio clocks never agree exactly, and a burst after a
 *   stall leaves too much queued. Above HIGH_WATER_MSEC, single sample frames are skipped at a rate of at
 *   most 1 in DRIFT_SKIP_INTERVAL until the queue is back at the target - inaudible, unlike trimming a
 *   chunk. Past MAX_BUFFER_MSEC (a long stall) it trims outright, because catching up gradually would take
 *   longer than the listener would tolerate the delay.
 *
 * addOpusPacket() decodes and is called on the main thread; prepareSampleBuffer() runs on the real-time
 * mixer thread and only copies already-decoded samples, so the mixer never waits on a decoder.
 */
class AudioOutputScreenShare : public AudioOutputBuffer {
private:
	Q_OBJECT
	Q_DISABLE_COPY(AudioOutputScreenShare)

public:
	/// @param mixerFreq The mixer's sample rate. Must be known (non-zero): resampling is decided here once.
	explicit AudioOutputScreenShare(unsigned int mixerFreq);
	~AudioOutputScreenShare() override;

	/// Decodes one Opus packet, concealing any packets missing before it, and queues the result.
	/// @param sequence The transport frame number: consecutive for consecutive packets of this stream.
	void addOpusPacket(std::uint64_t sequence, const QByteArray &opusPacket);

	/// Always reports itself alive: unlike a sound file, there is no natural end this class can detect on
	/// its own. The stream's actual end arrives out-of-band, as a VideoState announcing active=false, and
	/// is handled by removing this buffer from AudioOutput directly rather than through this return value.
	bool prepareSampleBuffer(unsigned int frameCount) override;

	float volumeMultiplier() const override { return m_volume.load(std::memory_order_relaxed); }

	/// Set from the UI thread - a per-tile volume slider in the video grid - and read from the audio
	/// mixing thread via volumeMultiplier() above.
	void setVolume(float multiplier) { m_volume.store(multiplier, std::memory_order_relaxed); }

	static constexpr unsigned int PREBUFFER_MSEC  = 60;
	static constexpr unsigned int HIGH_WATER_MSEC = 140;
	static constexpr unsigned int MAX_BUFFER_MSEC = 400;
	/// At most one sample frame skipped per this many played: a 2% speed-up at worst.
	static constexpr unsigned int DRIFT_SKIP_INTERVAL = 50;
	/// Longest run of missing packets worth concealing; beyond it the gap is simply silence.
	static constexpr std::uint64_t MAX_CONCEALED_PACKETS = 5;
	/// How long a packet that arrived ahead of its predecessor waits for it before the predecessor is
	/// given up on. Network jitter routinely swaps neighbouring 20 ms packets; treating every swap as a
	/// loss conceals audio that was about to arrive and then has to throw the real packet away.
	static constexpr qint64 REORDER_WAIT_MSEC = 40;
	/// Packets held for reordering before a gap is given up on regardless of time.
	static constexpr std::size_t MAX_REORDER_PACKETS = 8;

protected:
	/// Decodes held packets in sequence order, concealing a gap once it has waited long enough.
	void drainReorderBuffer(qint64 nowMsec);
	/// Decodes one in-order packet, after concealing @p missingBefore packets immediately preceding it.
	void decodeInOrder(std::uint64_t sequence, const QByteArray &opusPacket, std::uint64_t missingBefore);
	/// Appends decoded 48 kHz stereo frames to the queue, resampled to the mixer rate if needed.
	void queueDecoded(const float *frames, int frameCount);
	/// Logs the counters below, at most every couple of seconds, when MUMBLE_VIDEO_STATS is set.
	void maybeLogStats();

	OpusDecoder *m_opusState         = nullptr;
	SpeexResamplerState *m_resampler = nullptr;
	unsigned int m_mixerFreq         = 0;

	/// Main-thread only.
	bool m_haveSequence             = false;
	std::uint64_t m_nextSequence    = 0;
	std::uint64_t m_highestSequence = 0;
	int m_lastFrameSize             = 960;

	struct HeldPacket {
		QByteArray data;
		qint64 arrivalMsec = 0;
	};
	/// Packets that arrived ahead of a missing predecessor, by sequence. Main-thread only.
	std::map< std::uint64_t, HeldPacket > m_reorder;

	QMutex m_mutex;

	/// Interleaved stereo float samples at m_mixerFreq. Read from m_readPos onwards; compacted from time to
	/// time rather than erased from the front on every mixer callback.
	std::vector< float > m_queue;
	std::size_t m_readPos        = 0;
	bool m_playing               = false;
	unsigned int m_skipCountdown = DRIFT_SKIP_INTERVAL;

	std::atomic< float > m_volume{ 1.0f };

	// Counters for MUMBLE_VIDEO_STATS; reset each time they are logged.
	bool m_statsEnabled               = false;
	std::uint64_t m_statsPackets      = 0;
	std::uint64_t m_statsConcealed    = 0;
	std::uint64_t m_statsFecRecovered = 0;
	std::uint64_t m_statsLate         = 0;
	std::uint64_t m_statsReordered    = 0;
	std::atomic< std::uint64_t > m_statsUnderruns{ 0 };
	std::atomic< std::uint64_t > m_statsDriftSkipped{ 0 };
	std::atomic< std::uint64_t > m_statsTrimmed{ 0 };
	qint64 m_statsLastLogMsec = 0;
};

#endif // MUMBLE_MUMBLE_AUDIOOUTPUTSCREENSHARE_H_
