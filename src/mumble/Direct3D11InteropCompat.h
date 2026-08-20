// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_DIRECT3D11INTEROPCOMPAT_H_
#define MUMBLE_MUMBLE_DIRECT3D11INTEROPCOMPAT_H_

#include "win.h"

#include <dxgi.h>
#include <inspectable.h>

/**
 * Stand-in for windows.graphics.directx.direct3d11.interop.h, for the two declarations
 * WgcWindowVideoSource.cpp actually needs from it. Not every MinGW-w64 packaging ships that header - this
 * project's own dev machine has it, but the Fedora-based cross-compile toolchain the CI uses does not,
 * even though it does ship windows.graphics.capture.h and windows.graphics.capture.interop.h, which need
 * only this file's two declarations to be usable. WgcWindowVideoSource.h falls back to this header via
 * __has_include rather than always using it, so a toolchain that does provide the real header keeps using
 * that one.
 *
 * Declared here to match the real header's public ABI exactly (interface layout, GUID, free function
 * signature and linkage) - this is talked to as raw COM and linked against a real system DLL export by
 * code neither this project nor MinGW controls, so getting it byte-for-byte right is load-bearing, not a
 * convenience.
 */

namespace Windows {
namespace Graphics {
namespace DirectX {
namespace Direct3D11 {

MIDL_INTERFACE("a9b3d012-3df2-4ee3-b8d1-8695f457d3c1")
IDirect3DDxgiInterfaceAccess : public IUnknown {
public:
	virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID iid, void **object) = 0;
};

} // namespace Direct3D11
} // namespace DirectX
} // namespace Graphics
} // namespace Windows

#ifdef __CRT_UUID_DECL
__CRT_UUID_DECL(::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess, 0xa9b3d012, 0x3df2, 0x4ee3,
				0xb8, 0xd1, 0x86, 0x95, 0xf4, 0x57, 0xd3, 0xc1)
#endif

extern "C" HRESULT WINAPI CreateDirect3D11DeviceFromDXGIDevice(IDXGIDevice *dxgiDevice, IInspectable **graphicsDevice);

#endif // MUMBLE_MUMBLE_DIRECT3D11INTEROPCOMPAT_H_
