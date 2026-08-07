// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "WasapiLoopbackSource.h"

#include <QtCore/QMetaObject>

#include <combaseapi.h>
#include <mmdeviceapi.h>

WasapiLoopbackSource::Worker::Worker(WasapiLoopbackSource *owner) : m_owner(owner) {
}

void WasapiLoopbackSource::Worker::run() {
	m_owner->runCaptureLoop();
}

WasapiLoopbackSource::WasapiLoopbackSource(QObject *parent) : AudioLoopbackSource(parent) {
}

WasapiLoopbackSource::~WasapiLoopbackSource() {
	WasapiLoopbackSource::stop();
}

bool WasapiLoopbackSource::start() {
	if (m_worker && m_worker->isRunning()) {
		return true;
	}

	delete m_worker;

	m_stopRequested = false;
	m_running       = false;
	m_worker        = new Worker(this);

	m_worker->start();

	// Like CameraVideoSource, this is asynchronous: WASAPI setup happens on the worker thread and a
	// genuine failure arrives afterwards through failed(), not as a false return here.
	return true;
}

void WasapiLoopbackSource::stop() {
	if (!m_worker) {
		return;
	}

	m_stopRequested = true;

	// Blocks until runCaptureLoop() actually returns. The capture loop polls this flag at most every
	// 200ms (see runCaptureLoop), so this is a bounded wait, not an indefinite one.
	m_worker->wait();

	delete m_worker;
	m_worker = nullptr;

	m_running = false;
}

bool WasapiLoopbackSource::isRunning() const {
	return m_running;
}

QString WasapiLoopbackSource::describe() const {
	// A fixed string rather than something derived from the actual resolved device: the device is looked
	// up on the capture thread, and reading a QString the capture thread might be concurrently writing -
	// from whatever thread calls describe(), typically the UI thread - would be a data race. A friendlier,
	// device-specific label is a UI improvement for later, not something correctness depends on now.
	return tr("System audio (default playback device)");
}

unsigned int WasapiLoopbackSource::sampleRate() const {
	return m_sampleRate;
}

unsigned int WasapiLoopbackSource::channelCount() const {
	return m_channelCount;
}

void WasapiLoopbackSource::runCaptureLoop() {
	// COINIT_MULTITHREADED, not the apartment-threaded default: this thread only ever talks to the audio
	// APIs it opens itself and never touches a UI object, so there is nothing here that needs a message
	// pump, which apartment threading would require.
	const HRESULT comInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	const bool comInitialized = SUCCEEDED(comInit);

	IMMDeviceEnumerator *enumerator     = nullptr;
	IMMDevice *device                   = nullptr;
	IAudioClient *client                = nullptr;
	IAudioCaptureClient *captureClient  = nullptr;
	WAVEFORMATEX *waveFormat            = nullptr;
	HANDLE hEvent                       = nullptr;

	// Local, not a member: everything here belongs to this one call of this function, and cleaning it up
	// through a single path keeps every early-return failure branch from having to repeat the teardown.
	const auto cleanup = [&]() {
		if (client) {
			client->Stop();
		}

		if (hEvent) {
			CloseHandle(hEvent);
			hEvent = nullptr;
		}

		if (waveFormat) {
			CoTaskMemFree(waveFormat);
			waveFormat = nullptr;
		}

		if (captureClient) {
			captureClient->Release();
			captureClient = nullptr;
		}

		if (client) {
			client->Release();
			client = nullptr;
		}

		if (device) {
			device->Release();
			device = nullptr;
		}

		if (enumerator) {
			enumerator->Release();
			enumerator = nullptr;
		}
	};

	const auto fail = [this](const QString &reason) {
		// Posted through the context-object overload of invokeMethod: if this WasapiLoopbackSource is
		// destroyed before the main thread gets to process this, Qt drops the call rather than running a
		// lambda that captured a now-dangling this. The same idiom CameraVideoSource uses for its own
		// asynchronous error reporting.
		QMetaObject::invokeMethod(this, [this, reason]() { emit failed(reason); }, Qt::QueuedConnection);
	};

	HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
								  reinterpret_cast< void ** >(&enumerator));

	if (FAILED(hr) || !enumerator) {
		cleanup();

		if (comInitialized) {
			CoUninitialize();
		}

		fail(tr("Could not create an audio device enumerator"));

		return;
	}

	hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);

	if (FAILED(hr) || !device) {
		cleanup();

		if (comInitialized) {
			CoUninitialize();
		}

		fail(tr("No default playback device is available to capture"));

		return;
	}

	hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast< void ** >(&client));

	if (FAILED(hr) || !client) {
		cleanup();

		if (comInitialized) {
			CoUninitialize();
		}

		fail(tr("Could not activate the playback device for capture"));

		return;
	}

	hr = client->GetMixFormat(&waveFormat);

	if (FAILED(hr) || !waveFormat) {
		cleanup();

		if (comInitialized) {
			CoUninitialize();
		}

		fail(tr("Could not read the playback device's audio format"));

		return;
	}

	bool isFloat = false;

	if (waveFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
		isFloat = true;
	} else if (waveFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE
			  && waveFormat->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
		const auto *extensible = reinterpret_cast< const WAVEFORMATEXTENSIBLE * >(waveFormat);
		isFloat                = extensible->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
	}

	// Shared-mode WASAPI mix formats are float in the overwhelming common case - the audio engine mixes
	// internally in float regardless of what any individual stream provides - but this is not guaranteed
	// by the API. Rather than also carrying an int16 path through the rest of this class and the Opus
	// encoder downstream for a case that in practice does not happen, an exotic device that reports
	// something else is reported as a failure instead of silently misinterpreted as float.
	if (!isFloat || waveFormat->wBitsPerSample != 32) {
		cleanup();

		if (comInitialized) {
			CoUninitialize();
		}

		fail(tr("The playback device's audio format is not one this build supports"));

		return;
	}

	const unsigned int channels = waveFormat->nChannels;
	const unsigned int rate     = waveFormat->nSamplesPerSec;

	hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
							0, 0, waveFormat, nullptr);

	if (FAILED(hr)) {
		cleanup();

		if (comInitialized) {
			CoUninitialize();
		}

		fail(tr("Could not initialise loopback capture on the playback device"));

		return;
	}

	hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

	if (!hEvent) {
		cleanup();

		if (comInitialized) {
			CoUninitialize();
		}

		fail(tr("Could not create a capture event"));

		return;
	}

	hr = client->SetEventHandle(hEvent);

	if (FAILED(hr)) {
		cleanup();

		if (comInitialized) {
			CoUninitialize();
		}

		fail(tr("Could not arm loopback capture"));

		return;
	}

	hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast< void ** >(&captureClient));

	if (FAILED(hr) || !captureClient) {
		cleanup();

		if (comInitialized) {
			CoUninitialize();
		}

		fail(tr("Could not obtain a capture service for the playback device"));

		return;
	}

	hr = client->Start();

	if (FAILED(hr)) {
		cleanup();

		if (comInitialized) {
			CoUninitialize();
		}

		fail(tr("Could not start loopback capture"));

		return;
	}

	m_sampleRate    = rate;
	m_channelCount  = channels;
	m_running       = true;
	m_clock.restart();

	while (!m_stopRequested) {
		// Bounded rather than infinite specifically so the stop flag gets checked periodically even if
		// the device never signals - a disconnected or hung device must not make stop() block forever.
		const DWORD waitResult = WaitForSingleObject(hEvent, 200);

		if (m_stopRequested) {
			break;
		}

		if (waitResult != WAIT_OBJECT_0) {
			continue;
		}

		UINT32 packetLength   = 0;
		HRESULT packetResult = captureClient->GetNextPacketSize(&packetLength);

		while (SUCCEEDED(packetResult) && packetLength > 0) {
			BYTE *data                 = nullptr;
			UINT32 numFramesAvailable = 0;
			DWORD flags               = 0;

			packetResult = captureClient->GetBuffer(&data, &numFramesAvailable, &flags, nullptr, nullptr);

			if (FAILED(packetResult)) {
				break;
			}

			const std::size_t byteCount = static_cast< std::size_t >(numFramesAvailable) * channels * sizeof(float);

			QByteArray pcm;

			if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
				// The device has nothing playing right now. Delivered as frames of zeros rather than
				// skipped: the Opus encoder downstream needs an unbroken stream to keep its own timing
				// consistent, not gaps it would have to detect and paper over itself.
				pcm = QByteArray(static_cast< int >(byteCount), '\0');
			} else {
				pcm = QByteArray(reinterpret_cast< const char * >(data), static_cast< int >(byteCount));
			}

			captureClient->ReleaseBuffer(numFramesAvailable);

			const std::uint64_t timestamp = static_cast< std::uint64_t >(m_clock.elapsed().count());

			QMetaObject::invokeMethod(
				this, [this, pcm, timestamp]() { emit samplesReady(pcm, timestamp); }, Qt::QueuedConnection);

			packetResult = captureClient->GetNextPacketSize(&packetLength);
		}
	}

	cleanup();

	if (comInitialized) {
		CoUninitialize();
	}

	m_running = false;
}
