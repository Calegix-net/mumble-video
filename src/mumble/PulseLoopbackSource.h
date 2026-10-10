// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_PULSELOOPBACKSOURCE_H_
#define MUMBLE_MUMBLE_PULSELOOPBACKSOURCE_H_

#include "AudioLoopbackSource.h"

#include <pulse/context.h>
#include <pulse/introspect.h>
#include <pulse/mainloop-api.h>
#include <pulse/stream.h>
#include <pulse/subscribe.h>
#include <pulse/thread-mainloop.h>

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>

class PulseAudio;

/**
 * Screen-share audio on Linux: everything the desktop is playing, except Mumble itself.
 *
 * Works against PulseAudio and against PipeWire's PulseAudio server alike, which between them are every
 * mainstream desktop. A plain recording of the output device's monitor would also pick up the other
 * participants' voices coming out of the same speakers and send them straight back into the share - an
 * echo of everyone to everyone - so instead each application's playback stream (sink input) is recorded
 * on its own through pa_stream_set_monitor_stream(), Mumble's own streams are skipped, and the rest are
 * mixed. The same "whole system minus this process tree" choice the Windows WASAPI source makes.
 *
 * All PulseAudio work happens on a threaded mainloop. Applications come and go and move between devices;
 * the sink-input subscription keeps the set of recordings in step. A mainloop timer mixes what has been
 * captured into one 48 kHz stereo stream every MIX_INTERVAL_MSEC, paced by the clock rather than by any
 * one application, so a paused player cannot stall the others and silence is still silence on the wire.
 * samplesReady() is therefore emitted on the mainloop thread; consumers connect with a queued connection,
 * as AudioLoopbackSource requires.
 */
class PulseLoopbackSource : public AudioLoopbackSource {
	Q_OBJECT

public:
	explicit PulseLoopbackSource(QObject *parent = nullptr);
	~PulseLoopbackSource() override;

	bool start() override;
	void stop() override;
	bool isRunning() const override { return m_running; }
	QString describe() const override;
	unsigned int sampleRate() const override { return CAPTURE_SAMPLE_RATE; }
	unsigned int channelCount() const override { return CAPTURE_CHANNELS; }

	/// Whether this system can provide per-application capture at all: libpulse loads and has
	/// pa_stream_set_monitor_stream.
	static bool isAvailable();

	// Not SAMPLE_RATE: Audio.h #defines that.
	static constexpr unsigned int CAPTURE_SAMPLE_RATE = 48000;
	static constexpr unsigned int CAPTURE_CHANNELS    = 2;
	static constexpr unsigned int MIX_INTERVAL_MSEC   = 10;
	/// Per-application backlog kept before the oldest audio is dropped: keeps a stalled mix from turning
	/// into a growing delay.
	static constexpr unsigned int MAX_BACKLOG_MSEC = 200;

private:
	struct Capture {
		pa_stream *stream       = nullptr;
		std::uint32_t sinkIndex = 0;
		std::deque< float > samples;
	};

	static void contextStateCallback(pa_context *context, void *userdata);
	static void subscribeCallback(pa_context *context, pa_subscription_event_type_t type, std::uint32_t index,
								  void *userdata);
	static void sinkInfoCallback(pa_context *context, const pa_sink_info *info, int eol, void *userdata);
	static void sinkInputInfoCallback(pa_context *context, const pa_sink_input_info *info, int eol, void *userdata);
	static void streamReadCallback(pa_stream *stream, std::size_t bytes, void *userdata);
	static void mixTimerCallback(pa_mainloop_api *api, pa_time_event *event, const struct timeval *tv, void *userdata);

	// Mainloop thread only.
	void refresh();
	void startCapture(std::uint32_t sinkInputIndex, std::uint32_t sinkIndex);
	void stopCapture(std::uint32_t sinkInputIndex);
	void mix();
	bool isOwnStream(const pa_sink_input_info *info) const;
	void scheduleMix();
	void fail(const QString &reason);

	std::unique_ptr< PulseAudio > m_pa;
	pa_threaded_mainloop *m_mainloop = nullptr;
	pa_context *m_context            = nullptr;
	pa_time_event *m_mixTimer        = nullptr;
	bool m_running                   = false;

	// Mainloop thread only.
	std::map< std::uint32_t, std::string > m_sinkMonitors;
	std::map< std::uint32_t, std::uint32_t > m_seenSinkInputs;
	std::map< std::uint32_t, Capture > m_captures;
	std::uint64_t m_mixStartUsec = 0;
	std::uint64_t m_mixedFrames  = 0;
};

#endif // MUMBLE_MUMBLE_PULSELOOPBACKSOURCE_H_
