// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "VideoEncoder.h"

#include <QtCore/QBuffer>
#include <QtCore/QByteArray>

#include <algorithm>

namespace {

/// Quality steps tried, in order, when a tile does not fit in one transport unit.
constexpr int QUALITY_FLOOR = 20;
constexpr int QUALITY_STEP  = 20;

std::uint64_t hashImage(const QImage &image) {
	// FNV-1a over the pixel rows. Rows are hashed individually because QImage may pad scanlines, and
	// the padding is not meaningful.
	std::uint64_t hash = 1469598103934665603ull;

	for (int y = 0; y < image.height(); ++y) {
		const uchar *scan       = image.constScanLine(y);
		const std::size_t bytes = static_cast< std::size_t >(image.width()) * 4u;

		for (std::size_t i = 0; i < bytes; ++i) {
			hash ^= scan[i];
			hash *= 1099511628211ull;
		}
	}

	return hash;
}

} // namespace

void TiledImageEncoder::setTileSize(int pixels) {
	m_tileSize = std::clamp(pixels, 16, 1024);
	reset();
}

void TiledImageEncoder::setQuality(int quality) {
	m_quality = std::clamp(quality, 1, 100);
	reset();
}

void TiledImageEncoder::reset() {
	m_tileHashes.clear();
	m_lastFrameSize = QSize();
}

std::vector< Mumble::Protocol::byte > TiledImageEncoder::encodeTile(const QImage &tile, bool &fitted,
																	bool &requantised) const {
	const std::size_t budget = Mumble::Protocol::VideoFragmenter::maxUnitSize();

	fitted      = false;
	requantised = false;

	for (int quality = m_quality; quality >= QUALITY_FLOOR; quality -= QUALITY_STEP) {
		QByteArray encoded;
		QBuffer buffer(&encoded);
		buffer.open(QIODevice::WriteOnly);

		if (!tile.save(&buffer, "JPEG", quality)) {
			break;
		}

		buffer.close();

		if (static_cast< std::size_t >(encoded.size()) <= budget) {
			fitted      = true;
			requantised = quality != m_quality;

			const auto *begin = reinterpret_cast< const Mumble::Protocol::byte * >(encoded.constData());

			return std::vector< Mumble::Protocol::byte >(begin, begin + encoded.size());
		}
	}

	return {};
}

bool TiledImageEncoder::encodeRegionSplitting(const QImage &source, int x, int y, int width, int height,
											  std::uint32_t streamID, std::uint64_t frameNumber,
											  std::uint64_t captureTimestampUsec, std::uint32_t &nextUnitID,
											  std::vector< EncodedVideoUnit > &units) {
	const QImage region = source.copy(x, y, width, height);

	bool fitted      = false;
	bool requantised = false;

	std::vector< Mumble::Protocol::byte > payload = encodeTile(region, fitted, requantised);

	if (fitted) {
		if (requantised) {
			m_lastStats.tilesRequantised++;
		}

		EncodedVideoUnit unit;
		unit.header.streamID             = streamID;
		unit.header.frameNumber          = frameNumber;
		unit.header.unitID               = nextUnitID++;
		unit.header.captureTimestampUsec = captureTimestampUsec;
		unit.header.isKeyframe           = true;
		unit.header.x                    = static_cast< std::uint32_t >(x);
		unit.header.y                    = static_cast< std::uint32_t >(y);
		unit.header.width                = static_cast< std::uint32_t >(width);
		unit.header.height               = static_cast< std::uint32_t >(height);
		unit.payload                     = std::move(payload);

		m_lastStats.bytesEncoded += unit.payload.size();
		m_lastStats.tilesEncoded++;

		units.push_back(std::move(unit));

		return true;
	}

	// Did not fit even at the quality floor. Split into quadrants and retry each independently, unless
	// this region is already too small for splitting to be worth attempting - in which case give up on
	// it exactly as before splitting existed, rather than recursing towards a one-pixel region forever.
	if (width < 2 * MIN_SPLIT_DIMENSION && height < 2 * MIN_SPLIT_DIMENSION) {
		m_lastStats.tilesDroppedOversize++;

		return false;
	}

	const int leftWidth  = (width + 1) / 2;
	const int topHeight  = (height + 1) / 2;
	const int rightWidth = width - leftWidth;
	const int botHeight  = height - topHeight;

	bool fullyCovered = true;

	// clang-format off
	fullyCovered = encodeRegionSplitting(source, x,             y,            leftWidth,  topHeight, streamID, frameNumber, captureTimestampUsec, nextUnitID, units) && fullyCovered;

	if (rightWidth > 0) {
		fullyCovered = encodeRegionSplitting(source, x + leftWidth, y,            rightWidth, topHeight, streamID, frameNumber, captureTimestampUsec, nextUnitID, units) && fullyCovered;
	}

	if (botHeight > 0) {
		fullyCovered = encodeRegionSplitting(source, x,             y + topHeight, leftWidth,  botHeight, streamID, frameNumber, captureTimestampUsec, nextUnitID, units) && fullyCovered;

		if (rightWidth > 0) {
			fullyCovered = encodeRegionSplitting(source, x + leftWidth, y + topHeight, rightWidth, botHeight, streamID, frameNumber, captureTimestampUsec, nextUnitID, units) && fullyCovered;
		}
	}
	// clang-format on

	return fullyCovered;
}

std::vector< EncodedVideoUnit > TiledImageEncoder::encode(const QImage &frame, std::uint32_t streamID,
														  std::uint64_t frameNumber, std::uint64_t captureTimestampUsec,
														  bool forceKeyframe) {
	m_lastStats = Stats();

	std::vector< EncodedVideoUnit > units;

	if (frame.isNull()) {
		return units;
	}

	// A resize invalidates the whole tile grid, so the comparison against the previous frame is
	// meaningless and everything has to be re-sent.
	if (frame.size() != m_lastFrameSize) {
		m_tileHashes.clear();
		m_lastFrameSize = frame.size();
		forceKeyframe   = true;
	}

	// QImage::copy on a frame whose format has padded or non-32-bit scanlines would make the per-tile
	// hashes depend on layout rather than content, so normalise once up front.
	const QImage source = frame.format() == QImage::Format_RGB32 ? frame : frame.convertToFormat(QImage::Format_RGB32);

	const int columns = (source.width() + m_tileSize - 1) / m_tileSize;
	const int rows    = (source.height() + m_tileSize - 1) / m_tileSize;

	const std::size_t tileCount = static_cast< std::size_t >(columns) * static_cast< std::size_t >(rows);

	if (m_tileHashes.size() != tileCount) {
		m_tileHashes.assign(tileCount, 0);
		forceKeyframe = true;
	}

	units.reserve(tileCount);

	// Unique across every unit this call emits, whole tiles and split quadrants alike - encodeRegionSplitting()
	// increments this once per piece. Unit ids only ever need to be distinct within one (stream, frame),
	// never tied to grid position, so a plain counter is simpler than deriving one from where a piece
	// came from.
	std::uint32_t nextUnitID = 0;

	for (int row = 0; row < rows; ++row) {
		for (int column = 0; column < columns; ++column) {
			const std::size_t index = static_cast< std::size_t >(row) * static_cast< std::size_t >(columns)
									  + static_cast< std::size_t >(column);

			const int x = column * m_tileSize;
			const int y = row * m_tileSize;
			const int w = std::min(m_tileSize, source.width() - x);
			const int h = std::min(m_tileSize, source.height() - y);

			const QImage tile = source.copy(x, y, w, h);

			m_lastStats.tilesConsidered++;

			const std::uint64_t hash = hashImage(tile);

			if (!forceKeyframe && m_tileHashes[index] == hash) {
				m_lastStats.tilesUnchanged++;
				continue;
			}

			// Encodes the tile whole if it fits, or splits it into independently-encoded quadrants if it
			// does not - see the class comment for why dropping outright is a last resort now, not the
			// first response to a tile being too large. The hash is recorded only when every quadrant
			// made it out, so a partially-covered tile keeps being retried rather than being mistaken for
			// done.
			const bool fullyCovered = encodeRegionSplitting(source, x, y, w, h, streamID, frameNumber,
															captureTimestampUsec, nextUnitID, units);

			if (fullyCovered) {
				m_tileHashes[index] = hash;
			}
		}
	}

	// Marks the frame boundary so a receiver can present without waiting for a timeout. Set on the last
	// unit actually produced, which is not necessarily the last tile of the grid.
	if (!units.empty()) {
		units.back().header.isFrameEnd = true;
	}

	return units;
}
