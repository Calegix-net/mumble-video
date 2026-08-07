// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "DxgiDisplayVideoSource.h"

#include <cstring>

namespace {

/// Feature levels to try, newest first. D3D11CreateDevice picks the first the adapter actually supports;
/// duplication itself only needs 10.0, but asking for the newer ones first costs nothing and lets the
/// device use them if present.
const D3D_FEATURE_LEVEL FEATURE_LEVELS[] = {
	D3D_FEATURE_LEVEL_11_1,
	D3D_FEATURE_LEVEL_11_0,
	D3D_FEATURE_LEVEL_10_1,
	D3D_FEATURE_LEVEL_10_0,
};

} // namespace

QList< DxgiDisplayVideoSource::DisplayInfo > DxgiDisplayVideoSource::availableDisplays() {
	QList< DisplayInfo > result;

	IDXGIFactory1 *factory = nullptr;

	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast< void ** >(&factory))) || !factory) {
		return result;
	}

	IDXGIAdapter1 *adapter = nullptr;

	for (UINT adapterIndex = 0; factory->EnumAdapters1(adapterIndex, &adapter) != DXGI_ERROR_NOT_FOUND;
		 ++adapterIndex) {
		IDXGIOutput *output = nullptr;

		for (UINT outputIndex = 0; adapter->EnumOutputs(outputIndex, &output) != DXGI_ERROR_NOT_FOUND;
			 ++outputIndex) {
			DXGI_OUTPUT_DESC desc;

			// AttachedToDesktop excludes an output DXGI knows about but that is not currently part of the
			// desktop - a monitor the OS has disabled, for instance. Duplicating one of those fails
			// outright, so there is nothing useful to offer for it in the picker.
			if (SUCCEEDED(output->GetDesc(&desc)) && desc.AttachedToDesktop) {
				DisplayInfo info;
				info.deviceName   = QString::fromWCharArray(desc.DeviceName);
				info.friendlyName = info.deviceName;
				info.geometry     = QRect(desc.DesktopCoordinates.left, desc.DesktopCoordinates.top,
										  desc.DesktopCoordinates.right - desc.DesktopCoordinates.left,
										  desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top);

				result.append(info);
			}

			output->Release();
			output = nullptr;
		}

		adapter->Release();
		adapter = nullptr;
	}

	factory->Release();

	return result;
}

DxgiDisplayVideoSource::DxgiDisplayVideoSource(const DisplayInfo &display, QObject *parent)
	: VideoSource(parent), m_display(display) {
	// Connected once, here, rather than in start(): start()/stop() may run more than once over this
	// object's life, and reconnecting on every start() would stack up duplicate connections, firing
	// pollFrame() more than once per timeout after the second start().
	connect(&m_pollTimer, &QTimer::timeout, this, &DxgiDisplayVideoSource::pollFrame);
}

DxgiDisplayVideoSource::~DxgiDisplayVideoSource() {
	DxgiDisplayVideoSource::stop();
}

bool DxgiDisplayVideoSource::isRunning() const {
	return m_running;
}

QString DxgiDisplayVideoSource::describe() const {
	return m_display.friendlyName.isEmpty() ? tr("No display") : m_display.friendlyName;
}

void DxgiDisplayVideoSource::releaseDuplication() {
	if (m_stagingTexture) {
		m_stagingTexture->Release();
		m_stagingTexture = nullptr;
	}

	m_stagingWidth  = 0;
	m_stagingHeight = 0;

	if (m_duplication) {
		m_duplication->Release();
		m_duplication = nullptr;
	}

	if (m_context) {
		m_context->Release();
		m_context = nullptr;
	}

	if (m_device) {
		m_device->Release();
		m_device = nullptr;
	}
}

bool DxgiDisplayVideoSource::acquireDuplication() {
	releaseDuplication();

	IDXGIFactory1 *factory = nullptr;

	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast< void ** >(&factory))) || !factory) {
		emit failed(tr("Could not create a DXGI factory"));

		return false;
	}

	IDXGIAdapter1 *matchedAdapter = nullptr;
	IDXGIOutput *matchedOutput    = nullptr;

	IDXGIAdapter1 *adapter = nullptr;

	for (UINT adapterIndex = 0;
		 !matchedOutput && factory->EnumAdapters1(adapterIndex, &adapter) != DXGI_ERROR_NOT_FOUND; ++adapterIndex) {
		IDXGIOutput *output = nullptr;

		for (UINT outputIndex = 0; adapter->EnumOutputs(outputIndex, &output) != DXGI_ERROR_NOT_FOUND;
			 ++outputIndex) {
			DXGI_OUTPUT_DESC desc;

			if (SUCCEEDED(output->GetDesc(&desc)) && QString::fromWCharArray(desc.DeviceName) == m_display.deviceName) {
				matchedAdapter = adapter;
				matchedAdapter->AddRef();
				matchedOutput = output;
				matchedOutput->AddRef();
				output->Release();

				break;
			}

			output->Release();
			output = nullptr;
		}

		adapter->Release();
		adapter = nullptr;
	}

	factory->Release();

	if (!matchedOutput) {
		emit failed(tr("The display \"%1\" is no longer available").arg(m_display.friendlyName));

		return false;
	}

	D3D_FEATURE_LEVEL obtainedLevel;
	const HRESULT deviceResult =
		D3D11CreateDevice(matchedAdapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, FEATURE_LEVELS,
						  static_cast< UINT >(sizeof(FEATURE_LEVELS) / sizeof(FEATURE_LEVELS[0])), D3D11_SDK_VERSION,
						  &m_device, &obtainedLevel, &m_context);

	if (FAILED(deviceResult) || !m_device) {
		matchedOutput->Release();
		matchedAdapter->Release();
		emit failed(tr("Could not create a Direct3D device for this display"));

		return false;
	}

	IDXGIOutput1 *output1 = nullptr;
	const HRESULT queryResult = matchedOutput->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast< void ** >(&output1));

	matchedOutput->Release();
	matchedAdapter->Release();

	if (FAILED(queryResult) || !output1) {
		releaseDuplication();
		emit failed(tr("This display does not support duplication"));

		return false;
	}

	const HRESULT duplicateResult = output1->DuplicateOutput(m_device, &m_duplication);

	output1->Release();

	if (FAILED(duplicateResult) || !m_duplication) {
		releaseDuplication();

		if (duplicateResult == DXGI_ERROR_UNSUPPORTED) {
			// The classic cause: a remote desktop session has no real GPU output to duplicate.
			emit failed(tr("This display cannot be captured - remote desktop sessions do not support it"));
		} else if (duplicateResult == E_ACCESSDENIED) {
			// A secure desktop (a UAC prompt, the lock screen) is active. Transient, but not something to
			// retry from inside this call.
			emit failed(tr("Capturing the display was refused, possibly by a secure desktop"));
		} else {
			emit failed(tr("Could not start capturing this display"));
		}

		return false;
	}

	return true;
}

bool DxgiDisplayVideoSource::start() {
	if (m_running) {
		return true;
	}

	if (m_display.deviceName.isEmpty()) {
		emit failed(tr("No display selected"));

		return false;
	}

	if (!acquireDuplication()) {
		return false;
	}

	m_running = true;
	m_clock.restart();

	// 30fps ceiling on how often AcquireNextFrame is polled. It blocks internally until something
	// changes or this interval elapses, so this is a floor on responsiveness, not a fixed frame rate.
	m_pollTimer.start(1000 / 30);

	return true;
}

void DxgiDisplayVideoSource::stop() {
	if (!m_running) {
		return;
	}

	m_pollTimer.stop();
	releaseDuplication();

	m_running = false;
}

void DxgiDisplayVideoSource::pollFrame() {
	if (!m_running || !m_duplication) {
		return;
	}

	DXGI_OUTDUPL_FRAME_INFO frameInfo;
	IDXGIResource *desktopResource = nullptr;

	const HRESULT acquireResult = m_duplication->AcquireNextFrame(0, &frameInfo, &desktopResource);

	if (acquireResult == DXGI_ERROR_WAIT_TIMEOUT) {
		// Nothing has changed since the last frame. The overwhelmingly common case for a mostly-static
		// desktop, and not something to report or retry differently.
		return;
	}

	if (acquireResult == DXGI_ERROR_ACCESS_LOST) {
		// The output's mode changed, a fullscreen-exclusive application took it, or similar. The
		// duplication handle itself is now permanently unusable; re-acquiring a fresh one is the
		// documented recovery, and often succeeds without the user noticing anything happened.
		if (!acquireDuplication()) {
			// acquireDuplication() already emitted failed() with a specific reason.
			m_pollTimer.stop();
			m_running = false;
		}

		return;
	}

	if (FAILED(acquireResult)) {
		m_pollTimer.stop();
		releaseDuplication();
		m_running = false;
		emit failed(tr("Lost the display capture unexpectedly"));

		return;
	}

	ID3D11Texture2D *acquiredTexture = nullptr;
	const HRESULT textureResult =
		desktopResource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast< void ** >(&acquiredTexture));

	desktopResource->Release();

	if (FAILED(textureResult) || !acquiredTexture) {
		m_duplication->ReleaseFrame();

		return;
	}

	D3D11_TEXTURE2D_DESC desc;
	acquiredTexture->GetDesc(&desc);

	if (!m_stagingTexture || m_stagingWidth != desc.Width || m_stagingHeight != desc.Height) {
		if (m_stagingTexture) {
			m_stagingTexture->Release();
			m_stagingTexture = nullptr;
		}

		D3D11_TEXTURE2D_DESC stagingDesc = desc;
		stagingDesc.Usage               = D3D11_USAGE_STAGING;
		stagingDesc.BindFlags           = 0;
		stagingDesc.CPUAccessFlags      = D3D11_CPU_ACCESS_READ;
		stagingDesc.MiscFlags           = 0;
		stagingDesc.MipLevels           = 1;
		stagingDesc.ArraySize           = 1;
		stagingDesc.SampleDesc.Count    = 1;
		stagingDesc.SampleDesc.Quality  = 0;

		const HRESULT stagingResult = m_device->CreateTexture2D(&stagingDesc, nullptr, &m_stagingTexture);

		if (FAILED(stagingResult) || !m_stagingTexture) {
			acquiredTexture->Release();
			m_duplication->ReleaseFrame();
			m_pollTimer.stop();
			releaseDuplication();
			m_running = false;
			emit failed(tr("Could not allocate a capture buffer for this display"));

			return;
		}

		m_stagingWidth  = desc.Width;
		m_stagingHeight = desc.Height;
	}

	m_context->CopyResource(m_stagingTexture, acquiredTexture);
	acquiredTexture->Release();

	D3D11_MAPPED_SUBRESOURCE mapped;
	const HRESULT mapResult = m_context->Map(m_stagingTexture, 0, D3D11_MAP_READ, 0, &mapped);

	if (FAILED(mapResult)) {
		m_duplication->ReleaseFrame();

		return;
	}

	// Desktop Duplication always produces DXGI_FORMAT_B8G8R8A8_UNORM - guaranteed by the API, not
	// something read off the texture description - which in memory is byte order B,G,R,A. On the
	// little-endian architectures Windows runs on, that is bit-for-bit the same as QImage::Format_ARGB32
	// (0xAARRGGBB as a 32-bit value), so this is a row-by-row copy rather than a per-pixel conversion.
	QImage frame(static_cast< int >(desc.Width), static_cast< int >(desc.Height), QImage::Format_ARGB32);

	const auto *src = static_cast< const unsigned char * >(mapped.pData);

	for (UINT row = 0; row < desc.Height; ++row) {
		std::memcpy(frame.scanLine(static_cast< int >(row)),
					src + static_cast< std::size_t >(row) * mapped.RowPitch,
					static_cast< std::size_t >(desc.Width) * 4);
	}

	m_context->Unmap(m_stagingTexture, 0);
	m_duplication->ReleaseFrame();

	emit frameReady(frame, static_cast< std::uint64_t >(m_clock.elapsed().count()));
}
