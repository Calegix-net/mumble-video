// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "WasapiProcessLoopbackSource.h"

#include "WasapiProcessLoopbackTypes.h"

#include <QtCore/QMetaObject>

#include <combaseapi.h>
#include <mmdeviceapi.h>
#include <objbase.h>
#include <propidl.h>

namespace {

/**
 * Bridges ActivateAudioInterfaceAsync's callback-based completion back to a synchronous wait on the
 * capture thread. Heap-allocated and reference-counted like any COM object, since the API holds its own
 * reference to it for as long as the activation is in flight. SetEvent()/WaitForSingleObject() is what
 * establishes the happens-before relationship that makes reading m_client/m_result after the wait safe
 * without further synchronization, even though ActivateCompleted may run on a thread pool thread neither
 * this object nor the capture thread controls.
 */
class ProcessLoopbackActivationHandler : public IActivateAudioInterfaceCompletionHandler {
public:
	ProcessLoopbackActivationHandler() : m_event(CreateEvent(nullptr, TRUE, FALSE, nullptr)) {
	}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
		if (!ppv) {
			return E_POINTER;
		}

		if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
			*ppv = static_cast< IActivateAudioInterfaceCompletionHandler * >(this);
			AddRef();

			return S_OK;
		}

		*ppv = nullptr;

		return E_NOINTERFACE;
	}

	ULONG STDMETHODCALLTYPE AddRef() override {
		return ++m_refCount;
	}

	ULONG STDMETHODCALLTYPE Release() override {
		const ULONG remaining = --m_refCount;

		if (remaining == 0) {
			delete this;
		}

		return remaining;
	}

	HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation *op) override {
		HRESULT activateResult       = E_FAIL;
		IUnknown *activatedInterface = nullptr;

		if (op) {
			op->GetActivateResult(&activateResult, &activatedInterface);
		}

		m_result = activateResult;

		if (SUCCEEDED(activateResult) && activatedInterface) {
			activatedInterface->QueryInterface(__uuidof(IAudioClient), reinterpret_cast< void ** >(&m_client));
			activatedInterface->Release();
		}

		SetEvent(m_event);

		return S_OK;
	}

	HANDLE eventHandle() const {
		return m_event;
	}

	HRESULT result() const {
		return m_result;
	}

	IAudioClient *takeClient() {
		IAudioClient *client = m_client;
		m_client             = nullptr;

		return client;
	}

protected:
	virtual ~ProcessLoopbackActivationHandler() {
		if (m_event) {
			CloseHandle(m_event);
		}

		if (m_client) {
			m_client->Release();
		}
	}

private:
	std::atomic< ULONG > m_refCount{ 1 };
	HANDLE m_event;
	HRESULT m_result       = E_FAIL;
	IAudioClient *m_client = nullptr;
};

} // namespace

WasapiProcessLoopbackSource::Worker::Worker(WasapiProcessLoopbackSource *owner) : m_owner(owner) {
}

void WasapiProcessLoopbackSource::Worker::run() {
	m_owner->runCaptureLoop();
}

WasapiProcessLoopbackSource::WasapiProcessLoopbackSource(unsigned long targetProcessId,
														  const QString &processDescription, bool excludeTargetTree,
														  QObject *parent)
	: AudioLoopbackSource(parent), m_targetProcessId(targetProcessId), m_excludeTargetTree(excludeTargetTree),
	  m_processDescription(processDescription) {
}

WasapiProcessLoopbackSource::~WasapiProcessLoopbackSource() {
	WasapiProcessLoopbackSource::stop();
}

bool WasapiProcessLoopbackSource::start() {
	if (m_worker && m_worker->isRunning()) {
		return true;
	}

	delete m_worker;

	m_stopRequested = false;
	m_running       = false;
	m_worker        = new Worker(this);

	m_worker->start();

	// Asynchronous, like WasapiLoopbackSource: activation and format negotiation happen on the worker
	// thread, and a genuine failure surfaces afterwards through failed(), not as a false return here.
	return true;
}

void WasapiProcessLoopbackSource::stop() {
	if (!m_worker) {
		return;
	}

	m_stopRequested = true;

	// Blocks until runCaptureLoop() actually returns. The capture loop polls this flag at most every
	// 200ms, so this is a bounded wait, not an indefinite one.
	m_worker->wait();

	delete m_worker;
	m_worker = nullptr;

	m_running = false;
}

bool WasapiProcessLoopbackSource::isRunning() const {
	return m_running;
}

QString WasapiProcessLoopbackSource::describe() const {
	if (!m_processDescription.isEmpty()) {
		return m_processDescription;
	}

	return m_excludeTargetTree ? tr("System audio (excluding Mumble)") : tr("One application's audio");
}

unsigned int WasapiProcessLoopbackSource::sampleRate() const {
	return kSampleRate;
}

unsigned int WasapiProcessLoopbackSource::channelCount() const {
	return kChannelCount;
}

void WasapiProcessLoopbackSource::runCaptureLoop() {
	// COINIT_MULTITHREADED: this thread only talks to the audio APIs it opens itself, never a UI object,
	// so nothing here needs the message pump apartment threading would require.
	const HRESULT comInit     = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	const bool comInitialized = SUCCEEDED(comInit);

	IAudioClient *client                           = nullptr;
	IAudioCaptureClient *captureClient             = nullptr;
	HANDLE hEvent                                  = nullptr;
	ProcessLoopbackActivationHandler *handler      = nullptr;
	IActivateAudioInterfaceAsyncOperation *asyncOp = nullptr;

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

		if (captureClient) {
			captureClient->Release();
			captureClient = nullptr;
		}

		if (client) {
			client->Release();
			client = nullptr;
		}

		if (asyncOp) {
			asyncOp->Release();
			asyncOp = nullptr;
		}

		if (handler) {
			handler->Release();
			handler = nullptr;
		}
	};

	const auto fail = [this](const QString &reason) {
		// Posted through the context-object overload of invokeMethod: if this source is destroyed before
		// the main thread processes this, Qt drops the call rather than running a lambda that captured a
		// now-dangling this. Same idiom as WasapiLoopbackSource/CameraVideoSource.
		QMetaObject::invokeMethod(this, [this, reason]() { emit failed(reason); }, Qt::QueuedConnection);
	};

	AUDIOCLIENT_ACTIVATION_PARAMS activationParams = {};
	activationParams.ActivationType                 = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
	activationParams.ProcessLoopbackParams.TargetProcessId = static_cast< DWORD >(m_targetProcessId);
	activationParams.ProcessLoopbackParams.ProcessLoopbackMode =
		m_excludeTargetTree ? PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE
							: PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

	PROPVARIANT activationPropVariant;
	PropVariantInit(&activationPropVariant);
	activationPropVariant.vt             = VT_BLOB;
	activationPropVariant.blob.cbSize    = sizeof(activationParams);
	activationPropVariant.blob.pBlobData = reinterpret_cast< BYTE * >(&activationParams);

	handler = new ProcessLoopbackActivationHandler();

	HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
											 &activationPropVariant, handler, &asyncOp);

	if (FAILED(hr) || !asyncOp) {
		cleanup();

		if (comInitialized) {
			CoUninitialize();
		}

		fail(tr("Could not begin activating per-application audio capture"));

		return;
	}

	// Bounded rather than infinite: a hung activation must not hang stop() forever, and this is
	// documented to complete within milliseconds in the ordinary case.
	const DWORD activationWaitResult = WaitForSingleObject(handler->eventHandle(), 5000);

	if (activationWaitResult != WAIT_OBJECT_0 || FAILED(handler->result())) {
		cleanup();

		if (comInitialized) {
			CoUninitialize();
		}

		fail(tr("Could not activate per-application audio capture for this process"));

		return;
	}

	client = handler->takeClient();

	if (!client) {
		cleanup();

		if (comInitialized) {
			CoUninitialize();
		}

		fail(tr("Per-application audio capture did not provide an audio client"));

		return;
	}

	WAVEFORMATEX waveFormat     = {};
	waveFormat.wFormatTag       = WAVE_FORMAT_IEEE_FLOAT;
	waveFormat.nChannels        = static_cast< WORD >(kChannelCount);
	waveFormat.nSamplesPerSec   = kSampleRate;
	waveFormat.wBitsPerSample   = 32;
	waveFormat.nBlockAlign      = static_cast< WORD >(waveFormat.nChannels * waveFormat.wBitsPerSample / 8);
	waveFormat.nAvgBytesPerSec  = waveFormat.nSamplesPerSec * waveFormat.nBlockAlign;
	waveFormat.cbSize           = 0;

	// 200ms, matching Microsoft's own ApplicationLoopback sample: a process-loopback client, unlike an
	// ordinary loopback client, is documented to need an explicit non-zero buffer duration here rather
	// than the 0 that lets the shared audio engine pick a default.
	hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
							200 * 10000, 0, &waveFormat, nullptr);

	if (FAILED(hr)) {
		cleanup();

		if (comInitialized) {
			CoUninitialize();
		}

		fail(tr("Could not initialise per-application audio capture"));

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

		fail(tr("Could not arm per-application audio capture"));

		return;
	}

	hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast< void ** >(&captureClient));

	if (FAILED(hr) || !captureClient) {
		cleanup();

		if (comInitialized) {
			CoUninitialize();
		}

		fail(tr("Could not obtain a capture service for per-application audio"));

		return;
	}

	hr = client->Start();

	if (FAILED(hr)) {
		cleanup();

		if (comInitialized) {
			CoUninitialize();
		}

		fail(tr("Could not start per-application audio capture"));

		return;
	}

	m_running = true;
	m_clock.restart();

	while (!m_stopRequested) {
		// Polled, not event-driven. An event-callback *loopback* stream is documented by Microsoft
		// never to signal its event (only a render stream on the same device would pump it), so a loop
		// that waits for WAIT_OBJECT_0 before reading captures nothing, forever, with no error anywhere -
		// which is exactly how screen-share audio shipped: healthy-looking and silent. The event is kept
		// because the per-process variant does fire it; either way the packet queue is drained every
		// pass. 10ms keeps latency well under one Opus frame and makes stop() a bounded wait.
		WaitForSingleObject(hEvent, 10);

		if (m_stopRequested) {
			break;
		}

		UINT32 packetLength  = 0;
		HRESULT packetResult = captureClient->GetNextPacketSize(&packetLength);

		while (SUCCEEDED(packetResult) && packetLength > 0) {
			BYTE *data                = nullptr;
			UINT32 numFramesAvailable = 0;
			DWORD flags               = 0;

			packetResult = captureClient->GetBuffer(&data, &numFramesAvailable, &flags, nullptr, nullptr);

			if (FAILED(packetResult)) {
				break;
			}

			const std::size_t byteCount =
				static_cast< std::size_t >(numFramesAvailable) * kChannelCount * sizeof(float);

			QByteArray pcm;

			if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
				// The target process has nothing playing right now. Delivered as frames of zeros rather
				// than skipped, same reasoning as WasapiLoopbackSource: the Opus encoder downstream needs
				// an unbroken stream, not gaps it would have to detect and paper over itself.
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
