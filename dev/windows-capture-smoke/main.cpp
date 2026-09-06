// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.
#include "DxgiDisplayVideoSource.h"
#include "WasapiProcessLoopbackSource.h"
#include "WgcWindowVideoSource.h"
#include <QApplication>
#include <QDataStream>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QPainter>
#include <QProcess>
#include <QThread>
#include <QWidget>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mmsystem.h>

static bool until(const std::function< bool() > &condition, int timeout = 12000) {
	QElapsedTimer timer;
	timer.start();
	while (!condition() && timer.elapsed() < timeout) {
		QApplication::processEvents();
		QThread::msleep(10);
	}
	return condition();
}
class Pattern : public QWidget {
	void paintEvent(QPaintEvent *) override {
		QPainter p(this);
		p.fillRect(rect(), Qt::white);
		p.fillRect(QRect(0, 0, 30, height()), Qt::red);
		p.fillRect(QRect(width() - 30, 0, 30, height()), Qt::green);
		p.fillRect(QRect(0, height() - 30, width(), 30), Qt::blue);
	}
};
int main(int argc, char **argv) {
	QApplication app(argc, argv);
	if (app.arguments().contains("--tone")) {
		QByteArray wav;
		QDataStream stream(&wav, QIODevice::WriteOnly);
		stream.setByteOrder(QDataStream::LittleEndian);
		const int samples = 44100 * 2;
		const int bytes   = samples * 2;
		stream.writeRawData("RIFF", 4);
		stream << quint32(36 + bytes);
		stream.writeRawData("WAVEfmt ", 8);
		stream << quint32(16) << quint16(1) << quint16(1) << quint32(44100) << quint32(88200) << quint16(2)
			   << quint16(16);
		stream.writeRawData("data", 4);
		stream << quint32(bytes);
		for (int i = 0; i < samples; ++i)
			stream << qint16(6000 * std::sin(i * 2 * 3.141592653589793 * 440 / 44100));
		if (!PlaySoundW(reinterpret_cast< LPCWSTR >(wav.constData()), nullptr, SND_MEMORY | SND_ASYNC | SND_LOOP))
			return 2;
		QThread::sleep(90);
		PlaySoundW(nullptr, nullptr, 0);
		return 0;
	}
	int failures = 0;
	auto check   = [&](bool ok, const char *name) {
		std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
		std::fflush(stdout);
		if (!ok)
			++failures;
	};
	Pattern window;
	window.setWindowTitle("Capture regression pattern");
	window.resize(320, 240);
	window.show();
	until([&] { return window.isVisible(); });
	WgcWindowVideoSource source({ reinterpret_cast< HWND >(window.winId()), window.windowTitle() }, true);
	QImage frame;
	QString error;
	QObject::connect(&source, &VideoSource::frameReady, [&](const QImage &image, std::uint64_t) { frame = image; });
	QObject::connect(&source, &VideoSource::failed, [&](const QString &reason) {
		error = reason;
		qWarning() << reason;
	});
	check(source.start() && until([&] { return !frame.isNull() || !error.isEmpty(); }) && error.isEmpty()
			  && !frame.isNull(),
		  "window starts and returns pixels");
	const QSize initial = frame.size();
	std::printf("INITIAL %dx%d\n", initial.width(), initial.height());
	frame.save("capture-initial.png");
	window.resize(640, 420);
	check(until([&] { return frame.width() > initial.width() && frame.height() > initial.height(); }), "window grows");
	std::printf("GROWN %dx%d\n", frame.width(), frame.height());
	frame.save("capture-grown.png");
	window.resize(200, 120);
	check(until([&] { return frame.width() < initial.width() && frame.height() < initial.height(); }),
		  "window shrinks");
	std::printf("SHRUNK %dx%d\n", frame.width(), frame.height());
	frame.save("capture-shrunk.png");
	check(error.isEmpty(), "window capture stays healthy through resizing");
	source.stop();
	auto displays = DxgiDisplayVideoSource::availableDisplays();
	check(!displays.isEmpty(), "display enumeration");
	if (!displays.isEmpty()) {
		DxgiDisplayVideoSource display(displays.first());
		frame = QImage();
		error.clear();
		QObject::connect(&display, &VideoSource::frameReady,
						 [&](const QImage &image, std::uint64_t) { frame = image; });
		QObject::connect(&display, &VideoSource::failed, [&](const QString &reason) {
			error = reason;
			qWarning() << reason;
		});
		check(display.start() && until([&] { return !frame.isNull() || !error.isEmpty(); }) && error.isEmpty()
				  && !frame.isNull(),
			  "display capture returns pixels");
		frame.save("capture-display.png");
		display.stop();
	}
	QProcess tone;
	tone.start(QCoreApplication::applicationFilePath(), { "--tone" });
	check(tone.waitForStarted(), "separate application plays test tone");
	// Include the tone's process tree to check non-silent PCM; also check that the
	// full-system route can activate while excluding this process tree.
	for (bool exclude : { false, true }) {
		const auto target = exclude ? static_cast< unsigned long >(GetCurrentProcessId())
									: static_cast< unsigned long >(tone.processId());
		// Excluding our own tree would correctly exclude the child tone too. Launching
		// the player independently is needed for the exclusion test; this pass checks
		// activation/packet delivery, while the include pass checks non-silent PCM.
		WasapiProcessLoopbackSource audio(target, QString(), exclude);
		error.clear();
		int packets  = 0;
		bool nonzero = false;
		QObject::connect(&audio, &AudioLoopbackSource::failed, [&](const QString &reason) {
			error = reason;
			qWarning() << reason;
		});
		QObject::connect(&audio, &AudioLoopbackSource::samplesReady, [&](const QByteArray &pcm, std::uint64_t) {
			++packets;
			for (qsizetype i = 0; i + 4 <= pcm.size(); i += 4) {
				float sample;
				std::memcpy(&sample, pcm.constData() + i, 4);
				if (std::abs(sample) > 0.001f)
					nonzero = true;
			}
		});
		audio.start();
		until([&] { return !error.isEmpty() || (exclude ? audio.isRunning() : nonzero); });
		check(error.isEmpty() && audio.isRunning(),
			  exclude ? "system audio activation excluding own process tree" : "application audio activation");
		if (!exclude)
			check(nonzero && packets > 0, "application audio delivers non-silent PCM");
		audio.stop();
	}
	tone.terminate();
	if (!tone.waitForFinished(3000)) {
		tone.kill();
		tone.waitForFinished();
	}
	std::printf("FAILURES %d\n", failures);
	std::fflush(stdout);
	return failures ? 1 : 0;
}
