// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_GRAPHICSCAPTURECOMPAT_H_
#define MUMBLE_MUMBLE_GRAPHICSCAPTURECOMPAT_H_

#include "win.h"

#include <inspectable.h>
#include <windows.graphics.directx.direct3d11.h>

/**
 * The Windows.Graphics.Capture declarations MinGW-w64 does not ship.
 *
 * mingw-w64 12.0.0's windows.graphics.capture.h declares only the IGraphicsCaptureSession family. The
 * four interfaces below, and the SizeInt32 struct they pass around, are simply absent - so window
 * capture builds against the MSVC Windows SDK and fails to compile with the cross-toolchain the CI
 * uses. (Note that windows.graphics.capture.interop.h *does* declare IGraphicsCaptureItemInterop, which
 * is a different, unrelated interface; a substring search will suggest the item interface is present
 * when it is not.)
 *
 * This is the same situation Direct3D11InteropCompat.h already handles for
 * windows.graphics.directx.direct3d11.interop.h, and it is handled the same way.
 *
 * WHAT IS LOAD-BEARING HERE. These are not convenience declarations: they describe the ABI of COM
 * objects created by the operating system. Two things must match the real Windows SDK exactly, and
 * neither produces a compile error when wrong:
 *
 *   - The IIDs. RoGetActivationFactory and QueryInterface match on them; a wrong one fails at runtime
 *     with E_NOINTERFACE, which surfaces as "Window capture is not available on this system".
 *   - The method order. These are pure virtual, so the compiler assigns vtable slots in declaration
 *     order and calls whatever the OS object has in that slot. A method inserted, omitted, or reordered
 *     relative to the real interface calls the wrong function with the wrong arguments.
 *
 * Methods this project never calls are still declared, in their real positions, for exactly that
 * reason - the padding is the point. They are named as in the SDK and left unused.
 *
 * Reference: the IDL these come from is public, in Windows.Graphics.Capture.idl in the Windows SDK.
 * If window capture ever fails at runtime with E_NOINTERFACE on a machine where it should work, this
 * file is the first place to look.
 */

namespace ABI {
namespace Windows {
namespace Graphics {

// windows.graphics.h is not shipped either; this struct is passed by value to Create() and returned by
// get_Size(), so its layout matters as much as the interfaces'.
#ifndef MUMBLE_ABI_SIZEINT32_DEFINED
#	define MUMBLE_ABI_SIZEINT32_DEFINED
struct SizeInt32 {
	INT32 Width;
	INT32 Height;
};
#endif

namespace Capture {

interface IGraphicsCaptureItem;
interface IDirect3D11CaptureFrame;
interface IDirect3D11CaptureFramePool;
interface IDirect3D11CaptureFramePoolStatics;
interface IGraphicsCaptureSession;

/**
 * A capture target - here always a window, created through IGraphicsCaptureItemInterop::CreateForWindow.
 *
 * The two event methods are declared but unused: the Closed event would be the clean way to learn that
 * the shared window went away, which WgcWindowVideoSource currently detects by polling IsWindow()
 * instead. Their slots have to exist regardless.
 */
MIDL_INTERFACE("79c3f95b-31f7-4ec2-a464-632ef5d30760")
IGraphicsCaptureItem : public IInspectable {
public:
	virtual HRESULT STDMETHODCALLTYPE get_DisplayName(HSTRING *value) = 0;
	virtual HRESULT STDMETHODCALLTYPE get_Size(ABI::Windows::Graphics::SizeInt32 *value) = 0;
	// ITypedEventHandler<GraphicsCaptureItem*, IInspectable*> * in the SDK. Declared as void * because
	// the handler template is not available here either, and this project never subscribes - only the
	// slot position matters.
	virtual HRESULT STDMETHODCALLTYPE add_Closed(void *handler, EventRegistrationToken *token) = 0;
	virtual HRESULT STDMETHODCALLTYPE remove_Closed(EventRegistrationToken token) = 0;
};

/// One captured frame. get_Surface is the only method used; the other two hold their slots.
MIDL_INTERFACE("fa50c623-38da-4b32-acf3-fa9734ad800e")
IDirect3D11CaptureFrame : public IInspectable {
public:
	virtual HRESULT STDMETHODCALLTYPE get_Surface(
		ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface **value) = 0;
	virtual HRESULT STDMETHODCALLTYPE get_SystemRelativeTime(ABI::Windows::Foundation::TimeSpan *value) = 0;
	virtual HRESULT STDMETHODCALLTYPE get_ContentSize(ABI::Windows::Graphics::SizeInt32 *value) = 0;
};

/**
 * The pool frames are drawn from. TryGetNextFrame and CreateCaptureSession are used; Recreate, the
 * FrameArrived event pair and get_DispatcherQueue are declared for their slots.
 *
 * FrameArrived would be the push-based alternative to the polling WgcWindowVideoSource does today.
 */
MIDL_INTERFACE("24eb6d22-1975-422e-82e7-780dbd8ddf24")
IDirect3D11CaptureFramePool : public IInspectable {
public:
	virtual HRESULT STDMETHODCALLTYPE Recreate(
		ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice *device,
		ABI::Windows::Graphics::DirectX::DirectXPixelFormat pixelFormat, INT32 numberOfBuffers,
		ABI::Windows::Graphics::SizeInt32 size) = 0;
	virtual HRESULT STDMETHODCALLTYPE TryGetNextFrame(IDirect3D11CaptureFrame **result) = 0;
	// ITypedEventHandler<Direct3D11CaptureFramePool*, IInspectable*> * in the SDK; see add_Closed above.
	virtual HRESULT STDMETHODCALLTYPE add_FrameArrived(void *handler, EventRegistrationToken *token) = 0;
	virtual HRESULT STDMETHODCALLTYPE remove_FrameArrived(EventRegistrationToken token) = 0;
	virtual HRESULT STDMETHODCALLTYPE CreateCaptureSession(IGraphicsCaptureItem *item,
														  IGraphicsCaptureSession **result) = 0;
	// IDispatcherQueue * in the SDK.
	virtual HRESULT STDMETHODCALLTYPE get_DispatcherQueue(void **value) = 0;
};

/**
 * Cursor-capture toggle, Windows 10 2004 and later.
 *
 * mingw-w64 forward-declares this one but never defines it, so it is as unusable as the four above.
 * WgcWindowVideoSource queries for it and carries on without it when absent, which is the correct
 * behaviour on older Windows regardless.
 */
MIDL_INTERFACE("2c39ae40-7d2e-5044-804e-8b6799d4cf9e")
IGraphicsCaptureSession2 : public IInspectable {
public:
	virtual HRESULT STDMETHODCALLTYPE get_IsCursorCaptureEnabled(boolean *value) = 0;
	virtual HRESULT STDMETHODCALLTYPE put_IsCursorCaptureEnabled(boolean value) = 0;
};

/// Activation factory for the pool. Create() is the only method on it.
MIDL_INTERFACE("7784056a-67aa-4d53-ae54-1088d5a8ca21")
IDirect3D11CaptureFramePoolStatics : public IInspectable {
public:
	virtual HRESULT STDMETHODCALLTYPE Create(
		ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice *device,
		ABI::Windows::Graphics::DirectX::DirectXPixelFormat pixelFormat, INT32 numberOfBuffers,
		ABI::Windows::Graphics::SizeInt32 size, IDirect3D11CaptureFramePool **result) = 0;
};

} // namespace Capture
} // namespace Graphics
} // namespace Windows
} // namespace ABI

#ifdef __CRT_UUID_DECL
__CRT_UUID_DECL(ABI::Windows::Graphics::Capture::IGraphicsCaptureItem, 0x79c3f95b, 0x31f7, 0x4ec2, 0xa4,
				0x64, 0x63, 0x2e, 0xf5, 0xd3, 0x07, 0x60)
__CRT_UUID_DECL(ABI::Windows::Graphics::Capture::IDirect3D11CaptureFrame, 0xfa50c623, 0x38da, 0x4b32, 0xac,
				0xf3, 0xfa, 0x97, 0x34, 0xad, 0x80, 0x0e)
__CRT_UUID_DECL(ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePool, 0x24eb6d22, 0x1975, 0x422e,
				0x82, 0xe7, 0x78, 0x0d, 0xbd, 0x8d, 0xdf, 0x24)
__CRT_UUID_DECL(ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePoolStatics, 0x7784056a, 0x67aa,
				0x4d53, 0xae, 0x54, 0x10, 0x88, 0xd5, 0xa8, 0xca, 0x21)
__CRT_UUID_DECL(ABI::Windows::Graphics::Capture::IGraphicsCaptureSession2, 0x2c39ae40, 0x7d2e, 0x5044,
				0x80, 0x4e, 0x8b, 0x67, 0x99, 0xd4, 0xcf, 0x9e)
#endif

#endif // MUMBLE_MUMBLE_GRAPHICSCAPTURECOMPAT_H_
