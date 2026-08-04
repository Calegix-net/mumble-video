// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "VideoSource.h"

#include <QtGui/QPainter>

#include <algorithm>
#include <cstdint>

SyntheticVideoSource::SyntheticVideoSource(int width, int height, QObject *parent)
	: VideoSource(parent), m_width(width), m_height(height) {
}

bool SyntheticVideoSource::start() {
	m_running = true;

	if (m_intervalMs > 0) {
		m_clock.restart();
		m_timer.start(m_intervalMs);
	}

	return true;
}

void SyntheticVideoSource::setInterval(int milliseconds) {
	m_intervalMs = milliseconds;

	QObject::disconnect(&m_timer, &QTimer::timeout, nullptr, nullptr);

	if (milliseconds <= 0) {
		m_timer.stop();

		return;
	}

	QObject::connect(&m_timer, &QTimer::timeout, this,
					 [this]() { pump(static_cast< std::uint64_t >(m_clock.elapsed().count())); });

	if (m_running) {
		m_clock.restart();
		m_timer.start(milliseconds);
	}
}

void SyntheticVideoSource::stop() {
	m_running = false;
	m_timer.stop();
}

QString SyntheticVideoSource::describe() const {
	return QStringLiteral("Synthetic %1x%2").arg(m_width).arg(m_height);
}

QImage SyntheticVideoSource::render(std::uint64_t frameIndex) const {
	QImage image(m_width, m_height, QImage::Format_RGB32);

	// A static background that survives between frames, so that a low change ratio really does leave
	// most tiles byte-identical and the encoder's dirty-tile detection has something to detect.
	for (int y = 0; y < m_height; ++y) {
		QRgb *scan = reinterpret_cast< QRgb * >(image.scanLine(y));

		for (int x = 0; x < m_width; ++x) {
			const int v = 40 + ((x * 160) / std::max(1, m_width)) + ((y * 50) / std::max(1, m_height));
			scan[x]     = qRgb(std::clamp(v, 0, 255), std::clamp(v - 20, 0, 255), std::clamp(v + 25, 0, 255));
		}
	}

	// Then repaint the changing fraction as one contiguous band at the top. Contiguous, not scattered:
	// a changed row every tenth line would touch every tile row and leave the encoder nothing to skip,
	// which is neither what screen content looks like nor a useful thing to model.
	const int changedRows = (m_height * std::clamp(m_changePercent, 0, 100)) / 100;

	for (int y = 0; y < changedRows; ++y) {
		QRgb *scan = reinterpret_cast< QRgb * >(image.scanLine(y));

		for (int x = 0; x < m_width; ++x) {
			// Properly decorrelated, via a splitmix64-style finaliser. A bare seed + x*constant is
			// linear in x, which renders as a smooth sawtooth ramp and compresses about as well as a
			// gradient -- useless for exercising the encoder's size handling.
			std::uint64_t h = frameIndex * UINT64_C(0x9E3779B97F4A7C15)
							  + static_cast< std::uint64_t >(y) * UINT64_C(0xD1B54A32D192ED03)
							  + static_cast< std::uint64_t >(x) * UINT64_C(0xA0761D6478BD642F);
			h ^= h >> 30;
			h *= UINT64_C(0xBF58476D1CE4E5B9);
			h ^= h >> 27;
			h *= UINT64_C(0x94D049BB133111EB);
			h ^= h >> 31;

			scan[x] = qRgb(static_cast< int >((h >> 16) & 0xFF), static_cast< int >((h >> 8) & 0xFF),
						   static_cast< int >(h & 0xFF));
		}
	}

	return image;
}

QImage SyntheticVideoSource::pump(std::uint64_t captureTimestampUsec) {
	const QImage image = render(m_next++);

	emit frameReady(image, captureTimestampUsec);

	return image;
}
