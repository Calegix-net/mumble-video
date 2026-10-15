// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_VERSION_H_
#define MUMBLE_VERSION_H_

#include "VersionNumber.h"

#include <QString>
#include <QVariant>

// The numeric half of the version handling lives in the core, as VersionNumber.h. What is left here
// parses and formats it with Qt.

#define MUMBLE_RELEASE MUMBLE_RELEASE_STRING

namespace Version {

QString getRelease();
full_t fromString(const QString &version);
full_t fromConfig(const QVariant &config);
QString toString(full_t version);
QString toConfigString(full_t version);
bool getComponents(Version::component_t &major, Version::component_t &minor, Version::component_t &patch,
				   const QString &version);

} // namespace Version

#endif
