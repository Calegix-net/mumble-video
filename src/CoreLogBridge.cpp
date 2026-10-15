// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "CoreLogBridge.h"

#include "CoreLog.h"

#include <QtCore/QtGlobal>

namespace {
void forwardToQt(Mumble::Core::LogLevel level, const char *message) {
	switch (level) {
		case Mumble::Core::LogLevel::Info:
			qInfo("%s", message);
			break;
		case Mumble::Core::LogLevel::Warning:
			qWarning("%s", message);
			break;
		case Mumble::Core::LogLevel::Fatal:
			qFatal("%s", message);
	}
}
} // namespace

void installCoreLogHandler() {
	Mumble::Core::setLogHandler(forwardToQt);
}
