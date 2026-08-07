// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_WASAPILOOPBACKSOURCE_H_
#define MUMBLE_MUMBLE_WASAPILOOPBACKSOURCE_H_

#include "AudioLoopbackSource.h"
#include "Timer.h"
#include "win.h"

#include <QtCore/QThread>

#include <atomic>

#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

/**
 * Captures system output (loopback) via WASAPI, independent of the microphone.
 *
 * WASAPI's loopback capture is not a separate API: it is the ordinary capture path, pointed at a render
 * (output) endpoint instead of a capture (input) one, with AUDCLNT_STREAMFLAGS_LOOPBACK set on
 * Initialize(). This project already uses that trick for echo cancellation (see WASAPI.cpp's "Echo"
 * device), but only as a side effect of the microphone capture thread running, and only to feed the echo
 * canceller - never encoded or sent anywhere on its own. This class exists because that path cannot be
 * reused as-is: it shares a thread and an event handle with mic capture, so a user could not share a
 * screen with audio without their microphone also being open. This duplicates just the loopback-specific
 * mechanics, mic-independent, so screen-share audio works whether or not AudioInput is even running.
 *
 * Captures the default render device: whatever Windows is currently sending to the speakers, the same
 * scope Discord's "include audio" defaults to. Capturing one specific application's audio would need the
 * newer per-process loopback API (ActivateAudioInterfaceAsync with AUDIOCLIENT_ACTIVATION_PARAMS), which
 * is a larger, separate piece of work and not required for a first version.
 */
class WasapiLoopbackSource : public AudioLoopbackSource {
	Q_OBJECT

public:
	explicit WasapiLoopbackSource(QObject *parent = nullptr);
	~WasapiLoopbackSource() override;

	bool start() override;
	void stop() override;
	bool isRunning() const override;
	QString describe() const override;

	unsigned int sampleRate() const override;
	unsigned int channelCount() const override;

protected:
	/// Runs the capture loop on its own OS thread. Everything WASAPI/COM-related lives entirely inside
	/// this class rather than being split across a QThread subclass, so that the audio-format members
	/// below are only ever written from the capture thread and only ever read from it too - the owning
	/// object communicates with it exclusively through start()/stop() and Qt's queued signals.
	class Worker : public QThread {
	public:
		explicit Worker(WasapiLoopbackSource *owner);

	protected:
		void run() override;

		WasapiLoopbackSource *m_owner;
	};

	friend class Worker;

	void runCaptureLoop();

	Worker *m_worker = nullptr;

	// Set once, by the capture thread, before the first samplesReady() emission; read only after
	// isRunning() has become true, which happens no earlier than that point.
	std::atomic< unsigned int > m_sampleRate{ 0 };
	std::atomic< unsigned int > m_channelCount{ 0 };

	std::atomic< bool > m_running{ false };
	std::atomic< bool > m_stopRequested{ false };

	Timer m_clock;
};

#endif // MUMBLE_MUMBLE_WASAPILOOPBACKSOURCE_H_
