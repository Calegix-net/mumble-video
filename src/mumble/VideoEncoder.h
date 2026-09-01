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
 *    impossible to diagnose from the receiving end, where it looks like packet loss - and for a
 *    genuinely high-entropy region (a busy game scene, say) even the lowest quality step can still not
 *    fit, in which case the tile is split into independently-encoded quadrants - each with its own
 *    x/y/width/height, which the wire format already carries per unit - and each quadrant is split
 *    again if it still does not fit. A smaller region compresses to fewer bytes at the same quality
 *    purely because it has fewer pixels to encode, so this always terminates well before
 *    MIN_SPLIT_DIMENSION for any real image content. Splitting rather than giving up is what keeps a
 *    tile from becoming a permanent, never-updating black square merely because its content is noisy -
 *    and re-attempting the same failing whole-tile encode every single frame forever, which is what
 *    dropping without splitting used to cost, is itself a real source of needless lag.
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

	/// Counts encode() calls, used only to stagger the periodic per-tile refresh below - see encode(). Not
	/// reset by an explicit forceKeyframe (a resize, a genuine keyframe request): those need every tile
	/// sent immediately regardless of where the staggered cycle happens to be, and letting the cycle run
	/// on undisturbed means an explicit refresh never has to fight the staggered one for which tiles get
	/// sent this frame.
	unsigned int m_frameCounter = 0;

	/// A tile whose content has not changed is still re-sent once every this many frames, on a schedule
	/// staggered by tile index rather than all at once - see encode(). Recovers a tile lost to real network
	/// loss without a receiver-side mechanism to even notice one is missing (TiledImage units carry no
	/// per-tile sequence or ack, unlike VP8's frame-continuity tracking), without concentrating that
	/// recovery traffic into one all-tiles-at-once burst large enough to risk congesting the connection it
	/// is trying to repair - a single burst of every tile in a 1080p frame is well over a hundred JPEG units
	/// landing in one frame interval, and if some of that burst is itself lost, the compounding cost is a
	/// human-visible stretch of corruption well past the nominal recovery window, not a single lost tile.
	///
	/// 60, not the 150 this was originally shipped with: at the default 30fps that is a 2 second worst-case
	/// recovery window instead of 5, still comfortably staggered rather than bursty (see encode()'s
	/// per-tile modulo schedule) so this is a real reduction in how long a lost tile stays visibly wrong,
	/// not a reintroduction of the bandwidth spike the staggering exists to avoid - the periodic-refresh
	/// traffic this produces is still exactly one extra tile's worth of bandwidth per frame it falls on,
	/// only more of those frames now carry one.
	static constexpr unsigned int FULL_REFRESH_INTERVAL_FRAMES = 60;

	Stats m_lastStats;

	/**
	 * Encodes one tile as JPEG, reducing quality until it fits within the transport's unit budget.
	 *
	 * @param fitted Set to true if the result fits, false if even the lowest quality was too large.
	 * @param requantised Set to true if the baseline quality had to be reduced.
	 */
	std::vector< Mumble::Protocol::byte > encodeTile(const QImage &tile, bool &fitted, bool &requantised) const;

	/// Below this edge length, in pixels, a region that still does not fit is dropped rather than split
	/// further. In practice never reached: a region this small compresses to a few dozen bytes at worst,
	/// far under any realistic transport budget, at any quality. Sharing setTileSize()'s own floor is
	/// coincidence, not a dependency between the two - this one just needs to be "clearly too small to be
	/// worth subdividing again", and that value already means exactly that.
	static constexpr int MIN_SPLIT_DIMENSION = 16;

	/**
	 * Encodes one rectangular region, splitting it into quadrants - each independently positioned and
	 * independently a complete JPEG, exactly like a top-level tile - whenever it does not fit whole, down
	 * to MIN_SPLIT_DIMENSION. Appends every piece that did fit to @p units, with @p streamID and
	 * @p frameNumber copied into each header and @p nextUnitID incremented once per piece so every unit
	 * in the frame gets a distinct id.
	 *
	 * @returns Whether every pixel of the region was covered by some emitted piece. False means at least
	 *   one quadrant had to be dropped, which the caller uses to decide whether the tile's hash may be
	 *   recorded as "successfully sent" - a partially-covered tile must be retried next frame, not treated
	 *   as done.
	 */
	bool encodeRegionSplitting(const QImage &source, int x, int y, int width, int height, std::uint32_t streamID,
							   std::uint64_t frameNumber, std::uint64_t captureTimestampUsec,
							   std::uint32_t &nextUnitID, std::vector< EncodedVideoUnit > &units);
};

#endif // MUMBLE_MUMBLE_VIDEOENCODER_H_
