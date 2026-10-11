// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_CORELOGBRIDGE_H_
#define MUMBLE_CORELOGBRIDGE_H_

/// Routes the protocol core's log messages (CoreLog.h) into qInfo/qWarning/qFatal, so they end up
/// wherever the application's Qt message handler puts everything else.
void installCoreLogHandler();

#endif // MUMBLE_CORELOGBRIDGE_H_
