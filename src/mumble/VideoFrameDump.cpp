// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "VideoFrameDump.h"

#include <QtCore/QDir>
#include <QtCore/QProcessEnvironment>
#include <QtGui/QImage>

#include <atomic>

namespace {

/// The variable's value if it names a directory that exists or could be made, and empty otherwise.
QString resolve(const char *variable) {
	const QString value = QProcessEnvironment::systemEnvironment().value(QLatin1String(variable));

	if (value.isEmpty()) {
		return QString();
	}

	QDir dir(value);

	if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
		qWarning("VideoFrameDump: %s is set to \"%s\", which cannot be created; frames are not being saved",
				 variable, qPrintable(value));
		return QString();
	}

	qWarning("VideoFrameDump: saving frames to %s (%s)", qPrintable(dir.absolutePath()), variable);

	return dir.absolutePath();
}

} // namespace

namespace VideoFrameDump {

const QString &senderDirectory() {
	// Function-local statics: resolved on the first frame rather than at load time, because an
	// environment variable read before main() has set up Qt's environment is not reliably there.
	static const QString directory = resolve("MUMBLE_VIDEO_DUMP_TX");
	return directory;
}

const QString &receiverDirectory() {
	static const QString directory = resolve("MUMBLE_VIDEO_DUMP_RX");
	return directory;
}

void write(const QString &directory, const char *prefix, std::uint32_t streamID, std::uint64_t frameNumber,
		   const QImage &image) {
	if (directory.isEmpty() || image.isNull()) {
		return;
	}

	const QString path = QStringLiteral("%1/%2-%3-%4.png")
							 .arg(directory)
							 .arg(QLatin1String(prefix))
							 .arg(streamID)
							 .arg(frameNumber, 9, 10, QLatin1Char('0'));

	if (!image.save(path, "PNG")) {
		// Atomic because this is reached from two threads: the encode thread saves what it is about to
		// encode and the GUI thread saves what it has just decoded. A plain bool here would be a data
		// race - harmless in effect, but a real one, and this file exists to be run under measurement
		// tools that would rightly complain.
		static std::atomic< bool > warned{ false };

		if (!warned.exchange(true)) {
			qWarning("VideoFrameDump: could not write %s; no further failures will be reported",
					 qPrintable(path));
		}
	}
}

} // namespace VideoFrameDump
