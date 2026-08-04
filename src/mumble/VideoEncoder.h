// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_VIDEOENCODER_H_
#define MUMBLE_MUMBLE_VIDEOENCODER_H_

#include "VideoFragmentation.h"

#include <QtGui/QImage>

#include <cstdint>
#include <vector>

/**
 * One encoded video unit, ready to hand to Mumble::Protocol::VideoFragmenter.
 */
struct EncodedVideoUnit {
	Mumble::Protocol::VideoUnitHeader header;
	std::vector< Mumble::Protocol::byte > payload;
};

/**
 * Encodes frames as independently decodable rectangular tiles, each a complete JPEG.
 *
 * This is the TiledImage codec from the protocol. It needs no new dependency on either side, because
 * QImage already decodes JPEG, and it is the only scheme in the codec enum that produces genuinely
 * independent units: VP8 and its successors have no decodable sub-frame region, so under a transport
 * whose units are capped well below a whole frame they cannot be used at all.
 *
 * Two properties matter more than compression ratio here:
 *
 *  - Unchanged tiles are not re-sent. For a camera almost everything changes every frame and this
 *    saves nothing, but the same encoder serves screen sharing, where most of the frame is static.
 *  - A tile that will not fit in one transport unit is re-encoded at lower quality rather than
 *    dropped. Frame data that silently vanishes because it was a few bytes too large is close to
 *    impossible to diagnose from the receiving end, where it looks like packet loss.
 */
class TiledImageEncoder {
public:
	struct Stats {
		unsigned int tilesConsidered      = 0;
		unsigned int tilesEncoded         = 0;
		unsigned int tilesUnchanged       = 0;
		unsigned int tilesRequantised     = 0;
		unsigned int tilesDroppedOversize = 0;
		std::size_t bytesEncoded          = 0;
	};

	TiledImageEncoder() = default;

	/**
	 * Edge length of a tile in pixels. Smaller tiles mean more units per frame and finer-grained loss,
	 * but more per-unit overhead. Measured against 1080p content, 128 keeps even dense text tiles
	 * comfortably inside one transport unit; 256 does not.
	 */
	void setTileSize(int pixels);
	int tileSize() const { return m_tileSize; }

	/**
	 * Baseline JPEG quality, 1-100. A tile that does not fit is retried below this.
	 */
	void setQuality(int quality);
	int quality() const { return m_quality; }

	/**
	 * Forgets what the previous frame looked like, so the next frame is encoded in full. Call this when
	 * a receiver asks for a keyframe, or when a new receiver subscribes.
	 */
	void reset();

	/**
	 * Encodes one frame.
	 *
	 * @param frame The image to encode. Its dimensions may change between calls; a change forces a full
	 *   frame, since the tile grid is no longer comparable.
	 * @param streamID Stream this frame belongs to, copied into every unit header.
	 * @param frameNumber Monotonically increasing frame counter for the stream.
	 * @param captureTimestampUsec Capture time, copied into every unit header.
	 * @param forceKeyframe Encode every tile even if unchanged.
	 * @returns The units to send. Empty if nothing changed, which is a normal outcome for static
	 *   screen content and means "the receiver's picture is still correct".
	 */
	std::vector< EncodedVideoUnit > encode(const QImage &frame, std::uint32_t streamID, std::uint64_t frameNumber,
										   std::uint64_t captureTimestampUsec, bool forceKeyframe = false);

	const Stats &lastStats() const { return m_lastStats; }

protected:
	int m_tileSize = 128;
	int m_quality  = 80;

	QSize m_lastFrameSize;
	// Hash per tile of the previous frame, in row-major tile order, used to skip unchanged tiles.
	std::vector< std::uint64_t > m_tileHashes;

	Stats m_lastStats;

	/**
	 * Encodes one tile as JPEG, reducing quality until it fits within the transport's unit budget.
	 *
	 * @param fitted Set to true if the result fits, false if even the lowest quality was too large.
	 * @param requantised Set to true if the baseline quality had to be reduced.
	 */
	std::vector< Mumble::Protocol::byte > encodeTile(const QImage &tile, bool &fitted, bool &requantised) const;
};

#endif // MUMBLE_MUMBLE_VIDEOENCODER_H_
