// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_WGCCAPTURECOMPAT_H_
#define MUMBLE_MUMBLE_WGCCAPTURECOMPAT_H_

#include "win.h"

#include <eventtoken.h>
#include <inspectable.h>
#include <windows.foundation.h>
#include <windows.graphics.directx.direct3d11.h>

/**
 * Stand-in for windows.graphics.capture.h and windows.graphics.capture.interop.h, used unconditionally
 * rather than only as a fallback (contrast Direct3D11InteropCompat.h, which only substitutes for a
 * header that is outright absent on some toolchains).
 *
 * Those two headers are present on the CI toolchain, but broken there in a way __has_include cannot
 * detect: windows.graphics.capture.interop.h includes windows.ui.composition.h before
 * windows.graphics.capture.h internally, and something about that ordering - or about whatever earlier
 * in the same toolchain's header chain sets WINDOWS_FOUNDATION_UNIVERSALAPICONTRACT_VERSION - leaves
 * IGraphicsCaptureItem and IDirect3D11CaptureFramePool undeclared even once the DirectXPixelFormat/
 * DirectXAlphaMode enum errors from that ordering bug are separately worked around. Rather than keep
 * fighting an internal dependency chain this project does not control, this declares exactly what
 * WgcWindowVideoSource.cpp actually calls, matching the real ABI (interface layout, GUIDs, method
 * signatures) exactly - these are WinRT interfaces with a stable, documented, MIDL-generated shape, so a
 * hand-written declaration is not "guessing", it is transcribing.
 *
 * A handful of parameters this project's code never passes real arguments for (the FrameArrived/Closed
 * event handlers, the DispatcherQueue accessor) are declared with a local placeholder pointer type
 * instead of the real WinRT generic delegate/interface types, to avoid also needing to hand-roll
 * ITypedEventHandler<T1,T2> and IDispatcherQueue for methods nothing here ever calls. This is safe: a
 * vtable slot's ABI shape depends only on it being a pointer, never on what it points to, and C++ access
 * to that slot goes through the same declared (placeholder) type on both the write side (never, here)
 * and the read side (this header), so there is no mismatch to have.
 */

// ABI::Windows::Foundation::TimeSpan comes from windows.foundation.h, included above - not redeclared
// here, since unlike the Capture-specific interfaces below, that one is not part of whatever is broken
// on the CI toolchain.

namespace ABI {
namespace Windows {
namespace Graphics {
struct SizeInt32 {
	INT32 Width;
	INT32 Height;
};
namespace Capture {

// Placeholders for event-handler and dispatcher-queue parameter types this project never constructs or
// dereferences - see the class comment above.
struct WgcFrameArrivedHandlerPlaceholder;
struct WgcClosedHandlerPlaceholder;
struct WgcDispatcherQueuePlaceholder;

MIDL_INTERFACE("fa50c623-38da-4b32-acf3-fa9734ad800e")
IDirect3D11CaptureFrame : public IInspectable {
public:
	virtual HRESULT STDMETHODCALLTYPE get_Surface(
		::ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface **value) = 0;
	virtual HRESULT STDMETHODCALLTYPE get_SystemRelativeTime(::ABI::Windows::Foundation::TimeSpan *value) = 0;
	virtual HRESULT STDMETHODCALLTYPE get_ContentSize(SizeInt32 *value) = 0;
};

MIDL_INTERFACE("79c3f95b-31f7-4ec2-a464-632ef5d30760")
IGraphicsCaptureItem : public IInspectable {
public:
	virtual HRESULT STDMETHODCALLTYPE get_DisplayName(HSTRING *value)                     = 0;
	virtual HRESULT STDMETHODCALLTYPE get_Size(SizeInt32 *value)                          = 0;
	virtual HRESULT STDMETHODCALLTYPE add_Closed(WgcClosedHandlerPlaceholder *handler,
												 EventRegistrationToken *token)            = 0;
	virtual HRESULT STDMETHODCALLTYPE remove_Closed(EventRegistrationToken token)          = 0;
};

MIDL_INTERFACE("814e42a9-f70f-4ad7-939b-fddcc6eb880d")
IGraphicsCaptureSession : public IInspectable {
public:
	virtual HRESULT STDMETHODCALLTYPE StartCapture() = 0;
};

MIDL_INTERFACE("2c39ae40-7d2e-5044-804e-8b6799d4cf9e")
IGraphicsCaptureSession2 : public IInspectable {
public:
	virtual HRESULT STDMETHODCALLTYPE get_IsCursorCaptureEnabled(boolean *value) = 0;
	virtual HRESULT STDMETHODCALLTYPE put_IsCursorCaptureEnabled(boolean value)  = 0;
};

MIDL_INTERFACE("24eb6d22-1975-422e-82e7-780dbd8ddf24")
IDirect3D11CaptureFramePool : public IInspectable {
public:
	virtual HRESULT STDMETHODCALLTYPE Recreate(::ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice *device,
											   ::ABI::Windows::Graphics::DirectX::DirectXPixelFormat pixel_format,
											   INT32 number_of_buffers, SizeInt32 size)                    = 0;
	virtual HRESULT STDMETHODCALLTYPE TryGetNextFrame(IDirect3D11CaptureFrame **result)                    = 0;
	virtual HRESULT STDMETHODCALLTYPE add_FrameArrived(WgcFrameArrivedHandlerPlaceholder *handler,
													   EventRegistrationToken *token)                       = 0;
	virtual HRESULT STDMETHODCALLTYPE remove_FrameArrived(EventRegistrationToken token)                     = 0;
	virtual HRESULT STDMETHODCALLTYPE CreateCaptureSession(IGraphicsCaptureItem *item,
														    IGraphicsCaptureSession **result)                = 0;
	virtual HRESULT STDMETHODCALLTYPE get_DispatcherQueue(WgcDispatcherQueuePlaceholder **value)             = 0;
};

MIDL_INTERFACE("7784056a-67aa-4d53-ae54-1088d5a8ca21")
IDirect3D11CaptureFramePoolStatics : public IInspectable {
public:
	virtual HRESULT STDMETHODCALLTYPE Create(::ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice *device,
											 ::ABI::Windows::Graphics::DirectX::DirectXPixelFormat pixel_format,
											 INT32 number_of_buffers, SizeInt32 size,
											 IDirect3D11CaptureFramePool **result) = 0;
};

} // namespace Capture
} // namespace Graphics
} // namespace Windows
} // namespace ABI

#ifdef __CRT_UUID_DECL
__CRT_UUID_DECL(ABI::Windows::Graphics::Capture::IDirect3D11CaptureFrame, 0xfa50c623, 0x38da, 0x4b32, 0xac, 0xf3,
				0xfa, 0x97, 0x34, 0xad, 0x80, 0x0e)
__CRT_UUID_DECL(ABI::Windows::Graphics::Capture::IGraphicsCaptureItem, 0x79c3f95b, 0x31f7, 0x4ec2, 0xa4, 0x64, 0x63,
				0x2e, 0xf5, 0xd3, 0x07, 0x60)
__CRT_UUID_DECL(ABI::Windows::Graphics::Capture::IGraphicsCaptureSession, 0x814e42a9, 0xf70f, 0x4ad7, 0x93, 0x9b,
				0xfd, 0xdc, 0xc6, 0xeb, 0x88, 0x0d)
__CRT_UUID_DECL(ABI::Windows::Graphics::Capture::IGraphicsCaptureSession2, 0x2c39ae40, 0x7d2e, 0x5044, 0x80, 0x4e,
				0x8b, 0x67, 0x99, 0xd4, 0xcf, 0x9e)
__CRT_UUID_DECL(ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePool, 0x24eb6d22, 0x1975, 0x422e, 0x82, 0xe7,
				0x78, 0x0d, 0xbd, 0x8d, 0xdf, 0x24)
__CRT_UUID_DECL(ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePoolStatics, 0x7784056a, 0x67aa, 0x4d53,
				0xae, 0x54, 0x10, 0x88, 0xd5, 0xa8, 0xca, 0x21)
#endif // __CRT_UUID_DECL

// IGraphicsCaptureItemInterop is declared outside the ABI::Windows::... tree in the real header too - not
// this file's own convention, but matched here to keep call sites (which use the unqualified name)
// working identically regardless of which of the two headers actually supplied it.
MIDL_INTERFACE("3628e81b-3cac-4c60-b7f4-23ce0e0c3356")
IGraphicsCaptureItemInterop : public IUnknown {
public:
	virtual HRESULT STDMETHODCALLTYPE CreateForWindow(HWND window, REFIID iid, void **result)   = 0;
	virtual HRESULT STDMETHODCALLTYPE CreateForMonitor(HMONITOR monitor, REFIID iid, void **result) = 0;
};

#ifdef __CRT_UUID_DECL
__CRT_UUID_DECL(IGraphicsCaptureItemInterop, 0x3628e81b, 0x3cac, 0x4c60, 0xb7, 0xf4, 0x23, 0xce, 0x0e, 0x0c, 0x33,
				0x56)
#endif // __CRT_UUID_DECL

#endif // MUMBLE_MUMBLE_WGCCAPTURECOMPAT_H_
