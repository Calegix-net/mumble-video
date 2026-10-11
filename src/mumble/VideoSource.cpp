// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "VideoSource.h"

#include <QtGui/QFont>
#include <QtGui/QLinearGradient>
#include <QtGui/QPainter>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

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
	return m_screenLike ? QStringLiteral("Mock screen %1x%2").arg(m_width).arg(m_height)
						: QStringLiteral("Synthetic %1x%2").arg(m_width).arg(m_height);
}

namespace {
// The pseudo desktop. Deliberately drawn with plain QPainter primitives and default fonts so it renders
// identically on a headless box with no fontconfig to speak of - the point is stable, varied, mostly
// static content with a few always-moving regions, not fidelity.
QImage renderFilmLike(int width, int height, std::uint64_t frameIndex) {
	QImage image(width, height, QImage::Format_RGB32);

	const double frame = static_cast< double >(frameIndex);
	const double t     = frame / 30.0;

	// A slow diagonal sweep of colour with a fine texture panning across it: everything moves, smoothly.
	// Every term is a function of x or of y alone (the diagonal one through sin(a + b) = sin a cos b +
	// cos a sin b), so it is computed once per column and once per row: a per-pixel sine made the mock
	// itself the bottleneck at 1080p, capping the frame rate it was meant to measure.
	std::vector< double > red(static_cast< std::size_t >(width)), sinU(red.size()), cosU(red.size()),
		textureX(red.size());
	std::vector< double > green(static_cast< std::size_t >(height)), sinV(green.size()), cosV(green.size()),
		textureY(green.size());

	for (int x = 0; x < width; ++x) {
		const double u                          = static_cast< double >(x) / width;
		red[static_cast< std::size_t >(x)]      = 128 + 90 * std::sin(6.0 * u + t);
		sinU[static_cast< std::size_t >(x)]     = std::sin(4.0 * u - 0.7 * t + 4.0);
		cosU[static_cast< std::size_t >(x)]     = std::cos(4.0 * u - 0.7 * t + 4.0);
		textureX[static_cast< std::size_t >(x)] = 18.0 * std::sin((x + frame * 3.0) * 0.11);
	}

	for (int y = 0; y < height; ++y) {
		const double v                          = static_cast< double >(y) / height;
		green[static_cast< std::size_t >(y)]    = 128 + 90 * std::sin(5.0 * v + 1.3 * t + 2.0);
		sinV[static_cast< std::size_t >(y)]     = std::sin(4.0 * v);
		cosV[static_cast< std::size_t >(y)]     = std::cos(4.0 * v);
		textureY[static_cast< std::size_t >(y)] = std::cos((y - frame * 2.0) * 0.07);
	}

	for (int y = 0; y < height; ++y) {
		QRgb *scan            = reinterpret_cast< QRgb * >(image.scanLine(y));
		const std::size_t row = static_cast< std::size_t >(y);

		for (int x = 0; x < width; ++x) {
			const std::size_t column = static_cast< std::size_t >(x);
			const double texture     = textureX[column] * textureY[row];
			const double blue        = 128 + 90 * (sinU[column] * cosV[row] + cosU[column] * sinV[row]);

			scan[x] = qRgb(std::clamp(static_cast< int >(red[column] + texture), 0, 255),
						   std::clamp(static_cast< int >(green[row] + texture), 0, 255),
						   std::clamp(static_cast< int >(blue + texture), 0, 255));
		}
	}

	// Shapes crossing the frame at different speeds, so there is motion to predict, not only change.
	QPainter painter(&image);
	painter.setRenderHint(QPainter::Antialiasing);

	for (int i = 0; i < 6; ++i) {
		const double phase = t * (0.4 + 0.15 * i) + i;
		const int size     = height / (6 + i);
		const int cx       = static_cast< int >((0.5 + 0.42 * std::sin(phase)) * width);
		const int cy       = static_cast< int >((0.5 + 0.38 * std::cos(phase * 1.3)) * height);

		painter.setPen(Qt::NoPen);
		painter.setBrush(QColor::fromHsv((i * 60 + static_cast< int >(t * 20)) % 360, 200, 230));
		if (i % 2 == 0) {
			painter.drawEllipse(QPoint(cx, cy), size, size);
		} else {
			painter.drawRect(cx - size, cy - size / 2, size * 2, size);
		}
	}

	return image;
}

QImage renderScreenLike(int width, int height, std::uint64_t frameIndex) {
	QImage image(width, height, QImage::Format_RGB32);
	image.fill(QColor(30, 34, 42));

	QPainter p(&image);
	p.setRenderHint(QPainter::Antialiasing, false);

	// Wallpaper: a static diagonal gradient, so the bulk of the tiles never change.
	QLinearGradient wallpaper(0, 0, width, height);
	wallpaper.setColorAt(0.0, QColor(36, 52, 84));
	wallpaper.setColorAt(1.0, QColor(18, 24, 38));
	p.fillRect(image.rect(), wallpaper);

	// Bottom bar with a clock that changes once a second at 15 fps, like a real panel.
	const int barHeight = std::max(24, height / 30);
	p.fillRect(QRect(0, height - barHeight, width, barHeight), QColor(20, 20, 24));
	p.setPen(QColor(220, 220, 220));
	QFont font = p.font();
	font.setPixelSize(std::max(12, barHeight * 2 / 3));
	p.setFont(font);
	const std::uint64_t seconds = frameIndex / 15;
	p.drawText(QRect(0, height - barHeight, width - 12, barHeight), Qt::AlignRight | Qt::AlignVCenter,
			   QStringLiteral("mock screen  %1:%2:%3")
				   .arg(seconds / 3600, 2, 10, QLatin1Char('0'))
				   .arg((seconds / 60) % 60, 2, 10, QLatin1Char('0'))
				   .arg(seconds % 60, 2, 10, QLatin1Char('0')));

	// A "terminal" window in the left two thirds whose text scrolls up one line every few frames. Only
	// the text area changes, and only when a new line arrives - the frame of the window is static.
	const QRect term(width / 20, height / 12, width * 3 / 5, height * 2 / 3);
	p.fillRect(term.adjusted(-2, -28, 2, 2), QColor(60, 60, 66));
	p.fillRect(term, QColor(12, 12, 14));
	p.setPen(QColor(200, 200, 200));
	p.drawText(QRect(term.left(), term.top() - 26, term.width(), 24), Qt::AlignLeft | Qt::AlignVCenter,
			   QStringLiteral("  build.log - tail -f"));
	QFont mono(QStringLiteral("monospace"));
	mono.setStyleHint(QFont::Monospace);
	mono.setPixelSize(std::max(11, height / 45));
	p.setFont(mono);
	p.setPen(QColor(120, 220, 120));
	const int lineHeight = mono.pixelSize() + 4;
	const int lines      = std::max(1, term.height() / lineHeight);
	const std::uint64_t firstLine = frameIndex / 4; // one new line every 4 frames
	for (int i = 0; i < lines; ++i) {
		const std::uint64_t n = firstLine + static_cast< std::uint64_t >(i);
		std::uint64_t h       = n * UINT64_C(0x9E3779B97F4A7C15);
		h ^= h >> 31;
		const QString line = QStringLiteral("[%1] %2 object %3/%4 ... %5")
								 .arg(n, 6, 10, QLatin1Char('0'))
								 .arg((h & 1) ? QStringLiteral("Building CXX") : QStringLiteral("Linking   CXX"))
								 .arg((h >> 8) % 400)
								 .arg(400)
								 .arg(QString(static_cast< int >(8 + (h >> 20) % 40), QLatin1Char('#')));
		p.drawText(term.left() + 6, term.top() + (i + 1) * lineHeight - 2, line);
	}

	// A block bouncing around the right third: the always-moving region, several tiles wide.
	const QRect arena(width * 2 / 3, height / 12, width * 3 / 10, height * 2 / 3);
	const int block = std::max(32, std::min(arena.width(), arena.height()) / 4);
	const int rangeX = std::max(1, arena.width() - block);
	const int rangeY = std::max(1, arena.height() - block);
	const auto bounce = [](std::uint64_t t, int range) {
		const std::uint64_t period = static_cast< std::uint64_t >(range) * 2;
		const std::uint64_t phase  = t % period;
		return static_cast< int >(phase < static_cast< std::uint64_t >(range) ? phase : period - phase);
	};
	const int bx = arena.left() + bounce(frameIndex * 7, rangeX);
	const int by = arena.top() + bounce(frameIndex * 5, rangeY);
	p.fillRect(arena, QColor(40, 44, 54));
	p.fillRect(QRect(bx, by, block, block), QColor(240, 120, 40));
	p.setPen(QColor(255, 255, 255));
	p.setFont(font);
	p.drawText(QRect(bx, by, block, block), Qt::AlignCenter, QString::number(frameIndex % 1000));

	// Frame counter, large, top right: lets a viewer's screenshot say exactly which frame it shows.
	QFont big = font;
	big.setPixelSize(std::max(16, height / 12));
	p.setFont(big);
	p.setPen(QColor(255, 255, 255, 200));
	p.drawText(QRect(0, 0, width - 12, height / 10), Qt::AlignRight | Qt::AlignVCenter,
			   QStringLiteral("#%1").arg(frameIndex));

	p.end();

	return image;
}
} // namespace

QImage SyntheticVideoSource::render(std::uint64_t frameIndex) const {
	if (m_filmLike) {
		return renderFilmLike(m_width, m_height, frameIndex);
	}

	if (m_screenLike) {
		return renderScreenLike(m_width, m_height, frameIndex);
	}

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
