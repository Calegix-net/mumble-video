// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "WgcWindowVideoSource.h"

#include <roapi.h>
#include <winstring.h>

#include <cstring>

using namespace ABI::Windows::Graphics;
using namespace ABI::Windows::Graphics::Capture;
using namespace ABI::Windows::Graphics::DirectX;
using namespace ABI::Windows::Graphics::DirectX::Direct3D11;

namespace {

const D3D_FEATURE_LEVEL FEATURE_LEVELS[] = {
	D3D_FEATURE_LEVEL_11_1,
	D3D_FEATURE_LEVEL_11_0,
	D3D_FEATURE_LEVEL_10_1,
	D3D_FEATURE_LEVEL_10_0,
};

BOOL CALLBACK enumWindowsProc(HWND hwnd, LPARAM lParam) {
	auto *result = reinterpret_cast< QList< WgcWindowVideoSource::WindowInfo > * >(lParam);

	if (!IsWindowVisible(hwnd)) {
		return TRUE;
	}

	// Tool windows (floating palettes, tooltips) are not the kind of thing Alt-Tab shows, and are not
	// the kind of thing a user means by "share this window" either.
	if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) {
		return TRUE;
	}

	// A window owned by another window - most dialogs - is not top-level in the Alt-Tab sense; its owner
	// is what should appear in the list.
	if (GetWindow(hwnd, GW_OWNER) != nullptr) {
		return TRUE;
	}

	wchar_t titleBuf[512];
	const int titleLength = GetWindowTextW(hwnd, titleBuf, static_cast< int >(std::size(titleBuf)));

	if (titleLength == 0) {
		// No title to show a user means nothing to pick from a list, whatever the window actually is.
		return TRUE;
	}

	WgcWindowVideoSource::WindowInfo info;
	info.handle = hwnd;
	info.title  = QString::fromWCharArray(titleBuf, titleLength);

	result->append(info);

	return TRUE;
}

} // namespace

QList< WgcWindowVideoSource::WindowInfo > WgcWindowVideoSource::availableWindows() {
	QList< WindowInfo > result;

	EnumWindows(enumWindowsProc, reinterpret_cast< LPARAM >(&result));

	return result;
}

WgcWindowVideoSource::WgcWindowVideoSource(const WindowInfo &window, bool captureCursor, QObject *parent)
	: VideoSource(parent), m_window(window), m_captureCursor(captureCursor) {
	// Connected once, here, not in start(): see DxgiDisplayVideoSource's identical reasoning - start()/
	// stop() may run more than once over this object's life.
	connect(&m_pollTimer, &QTimer::timeout, this, &WgcWindowVideoSource::pollFrame);
}

WgcWindowVideoSource::~WgcWindowVideoSource() {
	WgcWindowVideoSource::stop();
}

bool WgcWindowVideoSource::isRunning() const {
	return m_running;
}

QString WgcWindowVideoSource::describe() const {
	return m_window.title.isEmpty() ? tr("No window") : m_window.title;
}

void WgcWindowVideoSource::releaseSession() {
	if (m_stagingTexture) {
		m_stagingTexture->Release();
		m_stagingTexture = nullptr;
	}

	m_stagingWidth  = 0;
	m_stagingHeight = 0;

	if (m_session) {
		m_session->Release();
		m_session = nullptr;
	}

	if (m_framePool) {
		m_framePool->Release();
		m_framePool = nullptr;
	}

	if (m_item) {
		m_item->Release();
		m_item = nullptr;
	}

	if (m_context) {
		m_context->Release();
		m_context = nullptr;
	}

	if (m_device) {
		m_device->Release();
		m_device = nullptr;
	}

	if (m_roInitialized) {
		RoUninitialize();
		m_roInitialized = false;
	}
}

bool WgcWindowVideoSource::acquireSession() {
	releaseSession();

	if (!IsWindow(m_window.handle)) {
		emit failed(tr("This window no longer exists"));

		return false;
	}

	const HRESULT roResult = RoInitialize(RO_INIT_MULTITHREADED);

	// RPC_E_CHANGED_MODE means some other component on this thread already initialised the Windows
	// Runtime differently - Qt's own event loop integration, most likely. Treated as already-available
	// rather than a failure, and specifically not uninitialised on the way out, since this object does
	// not own that initialisation and tearing it down would pull the rug out from under whatever does.
	if (SUCCEEDED(roResult)) {
		m_roInitialized = true;
	} else if (roResult != RPC_E_CHANGED_MODE) {
		emit failed(tr("Could not initialise the Windows Runtime"));

		return false;
	}

	HSTRING itemClassId = nullptr;
	const wchar_t itemClassName[] = L"Windows.Graphics.Capture.GraphicsCaptureItem";
	HRESULT hr = WindowsCreateString(itemClassName, static_cast< UINT32 >(wcslen(itemClassName)), &itemClassId);

	if (FAILED(hr)) {
		releaseSession();
		emit failed(tr("Could not resolve the screen-capture runtime class"));

		return false;
	}

	IGraphicsCaptureItemInterop *interop = nullptr;
	hr = RoGetActivationFactory(itemClassId, __uuidof(IGraphicsCaptureItemInterop),
								reinterpret_cast< void ** >(&interop));
	WindowsDeleteString(itemClassId);

	if (FAILED(hr) || !interop) {
		releaseSession();
		emit failed(tr("Window capture is not available on this system"));

		return false;
	}

	hr = interop->CreateForWindow(m_window.handle, __uuidof(IGraphicsCaptureItem),
								  reinterpret_cast< void ** >(&m_item));
	interop->Release();

	if (FAILED(hr) || !m_item) {
		releaseSession();
		emit failed(tr("Could not begin capturing this window"));

		return false;
	}

	SizeInt32 itemSize;
	m_item->get_Size(&itemSize);

	if (itemSize.Width <= 0 || itemSize.Height <= 0) {
		releaseSession();
		emit failed(tr("This window has no visible content to capture"));

		return false;
	}

	// BGRA support is required for a D3D11 device to interoperate with Direct3D11CaptureFramePool at
	// all - CreateDirect3D11DeviceFromDXGIDevice fails without it. A separate device from
	// DxgiDisplayVideoSource's own, since a window capture and a display capture may run concurrently
	// (someone sharing their camera's window alongside their screen, however unlikely) and sharing one
	// device across two independent capture lifetimes would tangle their teardown.
	D3D_FEATURE_LEVEL obtainedLevel;
	hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
						   FEATURE_LEVELS, static_cast< UINT >(sizeof(FEATURE_LEVELS) / sizeof(FEATURE_LEVELS[0])),
						   D3D11_SDK_VERSION, &m_device, &obtainedLevel, &m_context);

	if (FAILED(hr) || !m_device) {
		releaseSession();
		emit failed(tr("Could not create a Direct3D device for window capture"));

		return false;
	}

	IDXGIDevice *dxgiDevice = nullptr;
	hr = m_device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast< void ** >(&dxgiDevice));

	if (FAILED(hr) || !dxgiDevice) {
		releaseSession();
		emit failed(tr("Could not obtain the DXGI device for window capture"));

		return false;
	}

	IInspectable *inspectableDevice = nullptr;
	hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice, &inspectableDevice);
	dxgiDevice->Release();

	if (FAILED(hr) || !inspectableDevice) {
		releaseSession();
		emit failed(tr("Could not bridge Direct3D to the capture runtime"));

		return false;
	}

	IDirect3DDevice *direct3dDevice = nullptr;
	hr = inspectableDevice->QueryInterface(__uuidof(IDirect3DDevice), reinterpret_cast< void ** >(&direct3dDevice));
	inspectableDevice->Release();

	if (FAILED(hr) || !direct3dDevice) {
		releaseSession();
		emit failed(tr("Could not bridge Direct3D to the capture runtime"));

		return false;
	}

	HSTRING poolClassId = nullptr;
	const wchar_t poolClassName[] = L"Windows.Graphics.Capture.Direct3D11CaptureFramePool";
	hr = WindowsCreateString(poolClassName, static_cast< UINT32 >(wcslen(poolClassName)), &poolClassId);

	if (FAILED(hr)) {
		direct3dDevice->Release();
		releaseSession();
		emit failed(tr("Could not resolve the frame pool runtime class"));

		return false;
	}

	IDirect3D11CaptureFramePoolStatics *poolStatics = nullptr;
	hr = RoGetActivationFactory(poolClassId, __uuidof(IDirect3D11CaptureFramePoolStatics),
								reinterpret_cast< void ** >(&poolStatics));
	WindowsDeleteString(poolClassId);

	if (FAILED(hr) || !poolStatics) {
		direct3dDevice->Release();
		releaseSession();
		emit failed(tr("Window capture is not available on this system"));

		return false;
	}

	// Two buffers: enough that a slow consumer does not force the producer to stall, without holding
	// more frames than this poll-driven source could ever plausibly need at once.
	hr = poolStatics->Create(direct3dDevice, DirectXPixelFormat_B8G8R8A8UIntNormalized, 2, itemSize, &m_framePool);
	poolStatics->Release();
	direct3dDevice->Release();

	if (FAILED(hr) || !m_framePool) {
		releaseSession();
		emit failed(tr("Could not create a frame pool for window capture"));

		return false;
	}

	hr = m_framePool->CreateCaptureSession(m_item, &m_session);

	if (FAILED(hr) || !m_session) {
		releaseSession();
		emit failed(tr("Could not start a capture session for this window"));

		return false;
	}

	// Best-effort: cursor toggling needs IGraphicsCaptureSession2 (Windows 10 2004+). Its absence is not
	// a failure - capture still works, just always including the cursor, which is what every OS version
	// before that one did regardless.
	{
		IGraphicsCaptureSession2 *session2 = nullptr;

		if (SUCCEEDED(m_session->QueryInterface(__uuidof(IGraphicsCaptureSession2),
												reinterpret_cast< void ** >(&session2)))
			&& session2) {
			session2->put_IsCursorCaptureEnabled(m_captureCursor ? TRUE : FALSE);
			session2->Release();
		}
	}

	hr = m_session->StartCapture();

	if (FAILED(hr)) {
		releaseSession();
		emit failed(tr("Could not start capturing this window"));

		return false;
	}

	return true;
}

bool WgcWindowVideoSource::start() {
	if (m_running) {
		return true;
	}

	if (!m_window.handle) {
		emit failed(tr("No window selected"));

		return false;
	}

	if (!acquireSession()) {
		return false;
	}

	m_running = true;
	m_clock.restart();

	// Same 30fps polling ceiling as DxgiDisplayVideoSource, for the same reason: TryGetNextFrame returns
	// promptly either way, so this bounds how often it is asked rather than how fast frames can arrive.
	m_pollTimer.start(1000 / 30);

	return true;
}

void WgcWindowVideoSource::stop() {
	if (!m_running) {
		return;
	}

	m_pollTimer.stop();
	releaseSession();

	m_running = false;
}

void WgcWindowVideoSource::pollFrame() {
	if (!m_running || !m_framePool) {
		return;
	}

	if (!IsWindow(m_window.handle)) {
		m_pollTimer.stop();
		releaseSession();
		m_running = false;
		emit failed(tr("The shared window was closed"));

		return;
	}

	IDirect3D11CaptureFrame *frame = nullptr;
	HRESULT hr                     = m_framePool->TryGetNextFrame(&frame);

	if (FAILED(hr) || !frame) {
		// Nothing new since the last poll - the common case for a mostly-static window, same reasoning
		// as DXGI_ERROR_WAIT_TIMEOUT in DxgiDisplayVideoSource.
		return;
	}

	IDirect3DSurface *surface = nullptr;
	hr                        = frame->get_Surface(&surface);

	if (FAILED(hr) || !surface) {
		frame->Release();

		return;
	}

	// Unlike everything else from the WGC headers, this one interface is declared in plain ::Windows::...
	// rather than ABI::Windows::... - windows.graphics.directx.direct3d11.interop.h does not wrap its
	// declarations in the ABI namespace the way windows.graphics.capture.h and
	// windows.graphics.directx.direct3d11.h do, so it needs its own fully-qualified name here.
	::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess *access = nullptr;
	hr = surface->QueryInterface(__uuidof(::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess),
								 reinterpret_cast< void ** >(&access));
	surface->Release();

	if (FAILED(hr) || !access) {
		frame->Release();

		return;
	}

	ID3D11Texture2D *texture = nullptr;
	hr                       = access->GetInterface(__uuidof(ID3D11Texture2D), reinterpret_cast< void ** >(&texture));
	access->Release();

	if (FAILED(hr) || !texture) {
		frame->Release();

		return;
	}

	D3D11_TEXTURE2D_DESC desc;
	texture->GetDesc(&desc);

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
			texture->Release();
			frame->Release();

			return;
		}

		m_stagingWidth  = desc.Width;
		m_stagingHeight = desc.Height;
	}

	m_context->CopyResource(m_stagingTexture, texture);
	texture->Release();
	frame->Release();

	D3D11_MAPPED_SUBRESOURCE mapped;
	hr = m_context->Map(m_stagingTexture, 0, D3D11_MAP_READ, 0, &mapped);

	if (FAILED(hr)) {
		return;
	}

	// Same B8G8R8A8 -> Format_ARGB32 reasoning as DxgiDisplayVideoSource: the pixel format requested at
	// frame pool creation makes this a row-by-row byte copy, not a per-pixel conversion.
	QImage image(static_cast< int >(desc.Width), static_cast< int >(desc.Height), QImage::Format_ARGB32);

	const auto *src = static_cast< const unsigned char * >(mapped.pData);

	for (UINT row = 0; row < desc.Height; ++row) {
		std::memcpy(image.scanLine(static_cast< int >(row)),
					src + static_cast< std::size_t >(row) * mapped.RowPitch,
					static_cast< std::size_t >(desc.Width) * 4);
	}

	m_context->Unmap(m_stagingTexture, 0);

	emit frameReady(image, static_cast< std::uint64_t >(m_clock.elapsed().count()));
}
