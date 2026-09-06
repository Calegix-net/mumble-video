// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license.
#ifndef MUMBLE_MUMBLE_PIPEWIRELIBRARY_H_
#define MUMBLE_MUMBLE_PIPEWIRELIBRARY_H_
#include <QLibrary>
#include <QStringList>

inline bool loadPipeWireLibrary(QLibrary &library) {
	// Prefer the runtime SONAME already linked by screen capture. An unversioned
	// development symlink can resolve to a second copy outside an application bundle.
	for (const auto &name : { "libpipewire-0.3.so.0", "libpipewire-0.3.so", "libpipewire.so" }) {
		library.setFileName(QLatin1String(name));
		if (library.load())
			return true;
	}
	return false;
}
#endif
