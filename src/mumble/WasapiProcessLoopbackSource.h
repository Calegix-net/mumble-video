// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_WASAPIPROCESSLOOPBACKSOURCE_H_
#define MUMBLE_MUMBLE_WASAPIPROCESSLOOPBACKSOURCE_H_

#include "AudioLoopbackSource.h"
#include "Timer.h"
#include "win.h"

#include <QtCore/QString>
#include <QtCore/QThread>

#include <atomic>
#include <cstdint>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

/**
 * Captures one process's (and its child processes') audio output only, via the per-process WASAPI
 * loopback activation Windows 10 2004+ added: ActivateAudioInterfaceAsync targeted at the virtual
 * "VAD\Process_Loopback" device, carrying an AUDIOCLIENT_ACTIVATION_PARAMS blob (see
 * WasapiProcessLoopbackTypes.h for why those types are hand-defined rather than included from the SDK -
 * MinGW-w64 has the activation API itself but not that header).
 *
 * This exists for the "just this window/app's audio" checkbox: WasapiLoopbackSource captures everything
 * the system is currently playing, which is the right scope for whole-screen sharing, but sharing one
 * window is not the same scope - a user sharing Paint should not also broadcast their music player.
 * Targeting one process rather than one window is a deliberate approximation: WASAPI has no concept of
 * "this window's audio", only "this process's audio" (and its child processes, so e.g. a browser tab's
 * separate render process is still included) - the same approximation Windows' own per-app volume mixer
 * makes.
 *
 * Unlike WasapiLoopbackSource, the mix format here is not queried from a device: a process-loopback
 * activation has no real device behind it to query, so the format is the fixed 44.1kHz/stereo/float32
 * this API is documented to always deliver, supplied to Initialize() rather than read from it.
 */
class WasapiProcessLoopbackSource : public AudioLoopbackSource {
	Q_OBJECT

public:
	/**
	 * @param targetProcessId The process whose audio (and that of its child processes) should be
	 *   captured.
	 * @param processDescription Human-readable label for describe(), typically the shared window's title
	 *   or its owning process's executable name.
	 */
	WasapiProcessLoopbackSource(unsigned long targetProcessId, const QString &processDescription,
								QObject *parent = nullptr);
	~WasapiProcessLoopbackSource() override;

	bool start() override;
	void stop() override;
	bool isRunning() const override;
	QString describe() const override;

	unsigned int sampleRate() const override;
	unsigned int channelCount() const override;

protected:
	// Same reasoning as WasapiLoopbackSource::Worker: everything WASAPI/COM-related lives inside this
	// class so the capture-thread-owned state below is only ever touched from that one thread.
	class Worker : public QThread {
	public:
		explicit Worker(WasapiProcessLoopbackSource *owner);

	protected:
		void run() override;

		WasapiProcessLoopbackSource *m_owner;
	};

	friend class Worker;

	void runCaptureLoop();

	unsigned long m_targetProcessId;
	QString m_processDescription;

	Worker *m_worker = nullptr;

	std::atomic< bool > m_running{ false };
	std::atomic< bool > m_stopRequested{ false };

	Timer m_clock;

	// Fixed by the activation API's own documentation, not discovered - see the class comment.
	static constexpr unsigned int kSampleRate   = 44100;
	static constexpr unsigned int kChannelCount = 2;
};

#endif // MUMBLE_MUMBLE_WASAPIPROCESSLOOPBACKSOURCE_H_
