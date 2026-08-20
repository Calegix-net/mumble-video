// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_WASAPIPROCESSLOOPBACKTYPES_H_
#define MUMBLE_MUMBLE_WASAPIPROCESSLOOPBACKTYPES_H_

#include "win.h"

/**
 * Per-process WASAPI loopback (Windows 10 2004+) is activated through the same
 * ActivateAudioInterfaceAsync/IActivateAudioInterfaceCompletionHandler pair MinGW-w64's mmdeviceapi.h
 * already declares - that part needs nothing extra. What it needs beyond that is a handful of types from
 * audioclientactivationparams.h, a header that ships with the real Windows SDK but not with MinGW-w64: the
 * activation type/mode enums and the params struct that gets packed into a PROPVARIANT blob and handed to
 * ActivateAudioInterfaceAsync to say "this activation is for one process's output, not a device".
 *
 * Defined here to match the documented public ABI exactly (field order, sizes and the fixed
 * VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK path string are all load-bearing - this is talked to as raw bytes
 * by code neither this project nor MinGW controls), guarded so that a future MinGW-w64 release that does
 * add audioclientactivationparams.h does not collide with this.
 */
#ifndef AUDIOCLIENT_ACTIVATION_TYPE_DEFAULT

typedef enum _AUDIOCLIENT_ACTIVATION_TYPE {
	AUDIOCLIENT_ACTIVATION_TYPE_DEFAULT          = 0,
	AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK = 1
} AUDIOCLIENT_ACTIVATION_TYPE;

typedef enum _PROCESS_LOOPBACK_MODE {
	PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE = 0,
	PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE = 1
} PROCESS_LOOPBACK_MODE;

typedef struct _AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS {
	DWORD TargetProcessId;
	PROCESS_LOOPBACK_MODE ProcessLoopbackMode;
} AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS;

typedef struct _AUDIOCLIENT_ACTIVATION_PARAMS {
	AUDIOCLIENT_ACTIVATION_TYPE ActivationType;
	union {
		AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS ProcessLoopbackParams;
	} DUMMYUNIONNAME;
} AUDIOCLIENT_ACTIVATION_PARAMS;

// The well-known "device path" ActivateAudioInterfaceAsync expects for a process-loopback activation -
// there is no real device behind it, this string is simply how the API is told which activation flavour
// is being requested. Verbatim from Microsoft's own ApplicationLoopback sample.
#define VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK L"VAD\\Process_Loopback"

#endif // AUDIOCLIENT_ACTIVATION_TYPE_DEFAULT

#endif // MUMBLE_MUMBLE_WASAPIPROCESSLOOPBACKTYPES_H_
