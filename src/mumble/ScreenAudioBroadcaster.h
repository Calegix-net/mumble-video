// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_SCREENAUDIOBROADCASTER_H_
#define MUMBLE_MUMBLE_SCREENAUDIOBROADCASTER_H_

#include "VideoFragmentation.h"

#include <QtCore/QByteArray>
#include <QtCore/QObject>
#include <QtCore/QString>

#include <cstdint>
#include <memory>
#include <vector>

struct OpusEncoder;
struct SpeexResamplerState_;
typedef struct SpeexResamplerState_ SpeexResamplerState;

class AudioLoopbackSource;

/**
 * Turns loopback audio into a stream of Opus-encoded units, ready to send on the video transport as an
 * OpusAudio-coded stream. The send-side sibling of VideoBroadcaster, but for a screen share's audio
 * instead of its picture.
 *
 * Deliberately not built on AudioInput's encoder. That machinery - Opus setup, frame bundling, VAD,
 * comfort noise, the global voice target - is entangled with the single global microphone instance in
 * ways that would need to be picked apart before any of it could be reused for a second, independent
 * audio source. The actual libopus call sequence is simple enough that duplicating just that part here is
 * far less risk than trying to carve a reusable path out of AudioInput.
 *
 * Always encodes stereo at 48kHz, resampling and channel-mixing whatever the capture source actually
 * delivers: Opus only supports mono or stereo, and fixing the output shape here means every downstream
 * piece - the receive side's decoder, the wire format - only ever has to handle one case.
 */
class ScreenAudioBroadcaster : public QObject {
	Q_OBJECT

public:
	explicit ScreenAudioBroadcaster(QObject *parent = nullptr);
	~ScreenAudioBroadcaster() override;

	/**
	 * Starts capturing and encoding from the given source, which this object takes ownership of.
	 *
	 * @param streamID Allocated by the caller (see VideoBroadcaster::setNextStreamID for why this is not
	 *   self-assigned): a session running a camera, a screen and this at once must not let any two of
	 *   them land on the same stream id.
	 * @returns Whether the source started.
	 */
	bool start(std::unique_ptr< AudioLoopbackSource > source, std::uint32_t streamID);

	void stop();

	bool isActive() const { return m_source != nullptr; }

	std::uint32_t streamID() const { return m_streamID; }

	QString describe() const;

	/// Target Opus bitrate. System audio, not voice: comfortably higher than a voice-tuned default is
	/// warranted, since this is one stream shared with however many people are watching, not per-listener.
	void configure(unsigned int bitrateKbps);

signals:
	/// One encoded unit, ready to fragment and send. Same shape as VideoBroadcaster::unitReady, so the
	/// owner can reuse one sendVideoUnit wiring pattern for both.
	void unitReady(const Mumble::Protocol::VideoUnitHeader &header, const QByteArray &payload);

	/// Capture started or stopped, so the UI can update its toggle.
	void activeChanged(bool active);

	/// Capture failed after starting: device removed, permission revoked, backend error.
	void failed(const QString &reason);

protected slots:
	void onSamplesReady(const QByteArray &pcm, std::uint64_t captureTimestampUsec);

protected:
	static constexpr unsigned int TARGET_SAMPLE_RATE = 48000;
	static constexpr unsigned int TARGET_CHANNELS     = 2;

	/// 20ms at 48kHz. One of Opus's fixed valid frame sizes, and the same frame duration the microphone
	/// path uses, which is a reasonable default for the same reason: latency low enough not to be
	/// noticeable, without paying per-packet overhead on every 2.5ms.
	static constexpr int OPUS_FRAME_SAMPLES = 960;

	std::unique_ptr< AudioLoopbackSource > m_source;

	OpusEncoder *m_opusState             = nullptr;
	SpeexResamplerState *m_resampler     = nullptr;
	unsigned int m_resamplerInputRate    = 0;
	unsigned int m_resamplerInputChannels = 0;

	/// Interleaved stereo float samples at TARGET_SAMPLE_RATE, awaiting a full OPUS_FRAME_SAMPLES frame.
	/// Carries over between onSamplesReady() calls, since WASAPI packet sizes rarely land on an exact
	/// Opus frame boundary.
	std::vector< float > m_accumulator;

	unsigned int m_bitrateKbps = 96;

	std::uint64_t m_frameNumber = 0;
	std::uint32_t m_streamID    = 0;

	/// Converts one delivery's worth of interleaved samples - at the source's current sample rate and
	/// channel count, read from m_source - to TARGET_SAMPLE_RATE/TARGET_CHANNELS, appending the result to
	/// m_accumulator. Channel mixing happens first, so the resampler only ever has to handle stereo
	/// regardless of what the source actually delivers.
	void appendResampledFrames(const float *samples, std::size_t frameCount);

	/// Encodes and emits every complete frame currently sitting in m_accumulator, leaving any remainder
	/// for next time.
	void encodeAccumulatedFrames();
};

#endif // MUMBLE_MUMBLE_SCREENAUDIOBROADCASTER_H_
