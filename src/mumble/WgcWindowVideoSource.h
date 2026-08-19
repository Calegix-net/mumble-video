// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_WGCWINDOWVIDEOSOURCE_H_
#define MUMBLE_MUMBLE_WGCWINDOWVIDEOSOURCE_H_

#include "Timer.h"
#include "VideoSource.h"
#include "win.h"

#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QTimer>

#include <d3d11.h>
#include <dxgi1_2.h>

// Included explicitly, ahead of the two headers below, because at least one MinGW-w64 packaging (the
// CI's) has an internal ordering bug: windows.graphics.capture.interop.h pulls in windows.ui.composition.h
// transitively, which uses DirectXPixelFormat/DirectXAlphaMode - both declared here - without including
// this header itself first. Forcing the include here (its own include guard makes this a no-op on a
// toolchain that already gets the ordering right) sidesteps that rather than depending on it.
#include <windows.graphics.directx.h>
#include <windows.graphics.capture.h>
#include <windows.graphics.capture.interop.h>

// Not every MinGW-w64 packaging ships this header - see Direct3D11InteropCompat.h for which toolchain
// that bit and why a fallback exists at all.
#if defined(__has_include) && __has_include(<windows.graphics.directx.direct3d11.interop.h>)
#	include <windows.graphics.directx.direct3d11.interop.h>
#else
#	include "Direct3D11InteropCompat.h"
#endif

/**
 * Captures a single window via the Windows.Graphics.Capture API.
 *
 * DXGI Desktop Duplication (DxgiDisplayVideoSource) has no concept of "one window" - it duplicates an
 * entire output, so a window sharing at all would mean either a hand-rolled crop to the window's
 * bounding rectangle, which shows whatever is on top of it too, or this: the OS's own per-window capture,
 * which composites correctly no matter what overlaps the window. WGC is a WinRT API; this project has no
 * C++/WinRT projection available under MinGW, so this talks to it through the raw ABI COM interfaces
 * MinGW-w64's headers do provide, the same style CreateDXGIFactory1/D3D11CreateDevice calls already use
 * elsewhere in this file's sibling, just with IInspectable-based interfaces and RoGetActivationFactory
 * standing in for a vtable lookup by class name instead of a straightforward DLL export.
 *
 * Frame delivery is polled (TryGetNextFrame on a QTimer) rather than driven off FrameArrived: the event
 * variant needs a COM object implementing a templated ITypedEventHandler, which is meaningfully more
 * interop boilerplate for a source that already has to poll DXGI the same way, and TryGetNextFrame is
 * documented to return cleanly (null) when nothing new has arrived.
 */
class WgcWindowVideoSource : public VideoSource {
	Q_OBJECT

public:
	struct WindowInfo {
		HWND handle = nullptr;
		QString title;
	};

	/**
	 * Windows this build considers shareable: visible, top-level, not a tool window, not owned by
	 * another window. The same filter Alt-Tab applies, so the list matches what a user already thinks of
	 * as "the windows on my screen" rather than surfacing every hidden helper window a running
	 * application happens to own.
	 */
	static QList< WindowInfo > availableWindows();

	explicit WgcWindowVideoSource(const WindowInfo &window, bool captureCursor, QObject *parent = nullptr);
	~WgcWindowVideoSource() override;

	bool start() override;
	void stop() override;
	bool isRunning() const override;
	QString describe() const override;

protected slots:
	void pollFrame();

protected:
	bool acquireSession();
	void releaseSession();

	WindowInfo m_window;
	bool m_captureCursor;

	ID3D11Device *m_device         = nullptr;
	ID3D11DeviceContext *m_context = nullptr;

	// WinRT objects, held through their ABI (IInspectable-derived) interfaces rather than C++/WinRT
	// wrapper types, since no C++/WinRT projection is available under this MinGW toolchain.
	ABI::Windows::Graphics::Capture::IGraphicsCaptureItem *m_item             = nullptr;
	ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePool *m_framePool = nullptr;
	ABI::Windows::Graphics::Capture::IGraphicsCaptureSession *m_session      = nullptr;

	ID3D11Texture2D *m_stagingTexture = nullptr;
	UINT m_stagingWidth               = 0;
	UINT m_stagingHeight              = 0;

	QTimer m_pollTimer;
	Timer m_clock;

	bool m_running        = false;
	bool m_roInitialized  = false;
};

#endif // MUMBLE_MUMBLE_WGCWINDOWVIDEOSOURCE_H_
