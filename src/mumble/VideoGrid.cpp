// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "VideoGrid.h"

#include "Mumble.pb.h"

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QDateTime>
#include <QtCore/QFutureWatcher>
#include <QtCore/QTimer>
#include <QtGui/QEnterEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPaintEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QSlider>
#include <QtWidgets/QToolButton>

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>
#include <vector>

namespace {

/**
 * The picture-in-picture window a fullscreened tile is actually shown in - a genuine top-level,
 * showFullScreen()'d window covering the whole monitor, not VideoGrid filling its own (dock-sized, not
 * screen-sized) rect the way an early version did. That approach could never fill more than whatever room
 * the video dock happened to have, which on any window smaller than the monitor is not "fullscreen" by
 * any definition a user watching, say, someone else's gameplay would recognise.
 *
 * Deliberately dumb: it owns no picture of its own. It is handed a provider that reads the focused tile's
 * current picture straight out of the grid at paint time, and reacts to Esc or a double-click by asking its
 * owner to leave fullscreen rather than closing itself - m_focus is the single source of truth for what is
 * fullscreened, in VideoGrid, and this window is just one of the things relayout() brings into line with it.
 *
 * Not holding a QImage of its own is a performance matter, not a stylistic one. An earlier version kept a
 * copy of the surface's canvas here, which - QImage being implicitly shared - meant the grid's own canvas
 * had two owners. The very next tile painted into that canvas then had to detach it: a full deep copy of
 * a canvas that for a screen share is the sender's entire desktop, up to MAX_SURFACE_WIDTH x
 * MAX_SURFACE_HEIGHT, some 30 MB. A busy screen share delivers dozens of tiles a second, so watching one
 * fullscreen cost the viewer's machine on the order of a gigabyte of memcpy per second plus the allocator
 * churn to match - which is exactly the "watching a screen share fullscreen grinds the machine to a halt"
 * report that led here. Reading the canvas at paint time only, through a handle that lives no longer than
 * the paint itself, never leaves a second owner behind for the next tile to trip over.
 *
 * Two further costs are kept in check for the same reason. Repaints are coalesced to at most
 * REPAINT_INTERVAL_MSEC apart (Qt's own update() coalescing only collapses requests made within one turn
 * of the event loop, and each incoming tile is its own turn), and each repaint is confined to the screen
 * region the changed tiles actually map to, so a 128-pixel tile changing on a 4K share re-scales a
 * 128-pixel patch of the monitor, not the whole monitor.
 */
class FullscreenVideoWindow : public QWidget {
public:
	/// Fills in the picture and label to show right now. The image handed back must be treated as a
	/// short-lived read-only view - see the class comment.
	using ContentProvider = std::function< void(QImage &image, QString &label) >;

	/// About 30 repaints a second at most. Screen shares are rarely sent faster than that, and a paint
	/// here is a real, CPU-side scale of the sender's whole desktop onto the whole monitor when everything
	/// is dirty - not something worth doing more often than the eye can tell apart.
	static constexpr int REPAINT_INTERVAL_MSEC = 33;

	explicit FullscreenVideoWindow(VideoGrid *owner, ContentProvider provider)
		: QWidget(owner, Qt::Window), m_owner(owner), m_provider(std::move(provider)) {
		setWindowTitle(tr("Mumble"));

		QPalette pal = palette();
		pal.setColor(QPalette::Window, Qt::black);
		setPalette(pal);
		setAutoFillBackground(true);

		m_repaintTimer = new QTimer(this);
		m_repaintTimer->setSingleShot(true);
		m_repaintTimer->setInterval(REPAINT_INTERVAL_MSEC);
		connect(m_repaintTimer, &QTimer::timeout, this, &FullscreenVideoWindow::flushRepaint);
	}

	/// The whole picture is different - a different tile was focused, or the stream restarted. Everything
	/// is repainted, on the next coalesced repaint.
	void contentReplaced() {
		m_wholeDirty = true;
		scheduleRepaint();
	}

	/// Only the given rectangle of the picture, in the picture's own pixel coordinates, changed.
	void contentChanged(const QRect &sourceRect) {
		if (!m_wholeDirty) {
			m_dirtySource |= sourceRect;
		}

		scheduleRepaint();
	}

protected:
	/// Where the picture of the given size lands on this window - centred, letterboxed, never distorted.
	/// One function, used by both paintEvent() and flushRepaint(), so the region a dirty tile is mapped to
	/// and the region that tile is then painted into can never disagree.
	QRect targetRectFor(const QSize &imageSize) const {
		const QSize scaled = imageSize.scaled(size(), Qt::KeepAspectRatio);

		return QRect((width() - scaled.width()) / 2, (height() - scaled.height()) / 2, scaled.width(),
					 scaled.height());
	}

	void scheduleRepaint() {
		if (!m_repaintTimer->isActive()) {
			m_repaintTimer->start();
		}
	}

	void flushRepaint() {
		QImage image;
		QString label;
		m_provider(image, label);

		const bool whole = m_wholeDirty || image.isNull() || image.size() != m_paintedImageSize;

		m_wholeDirty = false;

		if (whole) {
			m_dirtySource = QRect();
			update();

			return;
		}

		if (m_dirtySource.isEmpty()) {
			return;
		}

		// Map the dirty picture rectangle onto the window, and pad it by a couple of pixels either side:
		// the scale is fractional, and the raster engine's sampling at a patch's edge has to agree with what
		// was painted last time around it, which it does as long as the patch is repainted a little wider
		// than the rounding could possibly have moved it.
		const QRect target = targetRectFor(image.size());
		const double scaleX = static_cast< double >(target.width()) / image.width();
		const double scaleY = static_cast< double >(target.height()) / image.height();

		const QRect dirty(static_cast< int >(std::floor(target.x() + m_dirtySource.x() * scaleX)) - 2,
						  static_cast< int >(std::floor(target.y() + m_dirtySource.y() * scaleY)) - 2,
						  static_cast< int >(std::ceil(m_dirtySource.width() * scaleX)) + 4,
						  static_cast< int >(std::ceil(m_dirtySource.height() * scaleY)) + 4);

		m_dirtySource = QRect();
		update(dirty.intersected(rect()));
	}

	void paintEvent(QPaintEvent *event) override {
		QImage image;
		QString label;
		m_provider(image, label);

		QPainter painter(this);
		painter.fillRect(event->rect(), Qt::black);

		if (!image.isNull()) {
			// Always the whole image at its full target rect, never a sub-rect of it: the clip region
			// paintEvent() is delivered with is what confines the work to the dirty patch, and drawing with
			// the same transform every time is what guarantees a patch's pixels are identical to those the
			// surrounding, untouched area was painted with.
			painter.drawImage(targetRectFor(image.size()), image);
		}

		m_paintedImageSize = image.size();

		const QRect margin = rect().adjusted(16, 12, -16, -12);

		if (!label.isEmpty()) {
			painter.setPen(Qt::white);
			painter.drawText(margin, Qt::AlignTop | Qt::AlignLeft, label);
		}

		painter.setPen(QColor(200, 200, 200));
		painter.drawText(margin, Qt::AlignBottom | Qt::AlignRight, tr("Esc or double-click to exit fullscreen"));
	}

	void resizeEvent(QResizeEvent *event) override {
		QWidget::resizeEvent(event);
		m_wholeDirty = true;
	}

	void keyPressEvent(QKeyEvent *event) override {
		if (event->key() == Qt::Key_Escape) {
			m_owner->clearFocusedTile();

			return;
		}

		QWidget::keyPressEvent(event);
	}

	void mouseDoubleClickEvent(QMouseEvent *) override { m_owner->clearFocusedTile(); }

private:
	VideoGrid *m_owner;
	ContentProvider m_provider;
	QTimer *m_repaintTimer = nullptr;

	/// Accumulated since the last repaint, in picture pixels. Ignored while m_wholeDirty is set.
	QRect m_dirtySource;
	bool m_wholeDirty = true;

	/// The picture size the last paint used. A picture that has since changed size - a surface growing to
	/// fit a tile at its edge, say - moves the whole target rect, so nothing painted before it is reusable.
	QSize m_paintedImageSize;
};

} // namespace

VideoGrid::VideoGrid(QWidget *parent) : QWidget(parent) {
	setAutoFillBackground(true);

	// Small floor, greedy ceiling: the panel must never be what stops the main window from being made
	// smaller, and given room it should take it. Tiles scale to whatever they get (see paintEvent).
	setMinimumSize(160, 90);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

	// Strong rather than the default NoFocus: Esc only reaches keyPressEvent() below if this widget
	// actually holds keyboard focus, and a click is the only way a user has told it they mean to interact
	// with a particular tile rather than whatever else is on screen.
	setFocusPolicy(Qt::StrongFocus);

	// Without this, mouseMoveEvent() only fires while a button is held down, which would make hover-reveal
	// controls require a click-and-hold to even discover. updateHoveredBar() is what actually uses it.
	setMouseTracking(true);

	// See checkForStaleStreams() and applyPollInterval(). A tenth of the tighter of the two timeouts:
	// frequent enough that a genuinely dead or merely-stalled stream is not left hanging noticeably past
	// whichever threshold applies to it, cheap enough that running it this often is not worth thinking
	// about further.
	m_staleCheckTimer = new QTimer(this);
	connect(m_staleCheckTimer, &QTimer::timeout, this, &VideoGrid::checkForStaleStreams);
	applyPollInterval();
}

void VideoGrid::setStaleStreamTimeoutMsecForTesting(int msec) {
	m_staleStreamTimeoutMsec = msec;
	applyPollInterval();
}

void VideoGrid::setStallRefreshTimeoutMsecForTesting(int msec) {
	m_stallRefreshTimeoutMsec = msec;
	applyPollInterval();
}

void VideoGrid::applyPollInterval() {
	m_staleCheckTimer->start(std::max(1, std::min(m_staleStreamTimeoutMsec, m_stallRefreshTimeoutMsec) / 10));
}

QSize VideoGrid::sizeHint() const {
	return QSize(640, 360);
}

int VideoGrid::senderCount() const {
	int count = 0;

	for (auto it = m_surfaces.cbegin(); it != m_surfaces.cend(); ++it) {
		// A surface not being watched occupies a slot the moment it is announced - it shows a preview
		// placeholder immediately, it does not wait on a canvas that opting out means will never fill.
		if (!it->second.watching || !it->second.canvas.isNull()) {
			++count;
		}
	}

	return count;
}

QImage VideoGrid::surfaceFor(unsigned int senderSession, unsigned int streamID) const {
	const auto it = m_surfaces.find(surfaceKey(senderSession, streamID));

	return it == m_surfaces.end() ? QImage() : it->second.canvas;
}

std::size_t VideoGrid::distinctSenderCount() const {
	// Small map, and this is only ever called on the announce path (once per stream start), not per
	// frame - a linear scan costs nothing worth avoiding here.
	std::unordered_map< unsigned int, bool > seen;

	for (auto it = m_surfaces.cbegin(); it != m_surfaces.cend(); ++it) {
		seen[it->second.senderSession] = true;
	}

	return seen.size();
}

std::vector< unsigned int > VideoGrid::senderSessions() const {
	std::vector< unsigned int > sessions;

	for (auto it = m_surfaces.cbegin(); it != m_surfaces.cend(); ++it) {
		// m_surfaces is keyed by (session, stream), so a sender holding both a camera and a screen appears
		// twice in this walk. Deduplicated here rather than left to the caller: every caller wants "who is
		// in this grid", and removeSender()'s per-session overload is a no-op the second time anyway.
		if (std::find(sessions.begin(), sessions.end(), it->second.senderSession) == sessions.end()) {
			sessions.push_back(it->second.senderSession);
		}
	}

	return sessions;
}

bool VideoGrid::growToFit(QImage &canvas, int x, int y, int width, int height) {
	if (width <= 0 || height <= 0) {
		return false;
	}

	// Checked in int arithmetic that cannot overflow, because these values come off the network.
	if (x < 0 || y < 0 || x > MAX_SURFACE_WIDTH - width || y > MAX_SURFACE_HEIGHT - height) {
		return false;
	}

	const int neededWidth  = std::max(canvas.width(), x + width);
	const int neededHeight = std::max(canvas.height(), y + height);

	if (neededWidth == canvas.width() && neededHeight == canvas.height()) {
		return true;
	}

	// Copied into a larger canvas rather than reallocated blank, so tiles already received survive a
	// resize. A sender whose resolution changes otherwise flashes back to black.
	QImage grown(neededWidth, neededHeight, QImage::Format_RGB32);
	grown.fill(Qt::black);

	if (!canvas.isNull()) {
		QPainter painter(&grown);
		painter.drawImage(0, 0, canvas);
	}

	canvas = grown;

	return true;
}

QImage VideoGrid::decodeJpegTile(const QByteArray &payload) {
	QImage tile;

	// Explicitly JPEG rather than letting Qt sniff the format. Sniffing would let a sender pick the
	// decoder by crafting a header, which turns every image format Qt supports into attack surface.
	if (!tile.loadFromData(payload, "JPEG")) {
		return QImage();
	}

	return tile;
}

QImage VideoGrid::decodeUnit(Surface &surface, const QByteArray &payload) {
	switch (surface.codec) {
		case MumbleProto::VideoState_Codec_TiledImage:
			return decodeJpegTile(payload);
		case MumbleProto::VideoState_Codec_VP8: {
			if (!surface.vp8) {
				surface.vp8 = std::make_unique< VP8Decoder >();
			}

			if (!surface.vp8->isValid()) {
				return QImage();
			}

			const auto *bytes = reinterpret_cast< const Mumble::Protocol::byte * >(payload.constData());

			return surface.vp8->decode(
				std::vector< Mumble::Protocol::byte >(bytes, bytes + static_cast< std::size_t >(payload.size())));
		}
		default:
			// CODEC_UNKNOWN, or a codec this build has no decoder for. Dropped rather than guessed at,
			// as the protocol requires.
			return QImage();
	}
}

void VideoGrid::setSenderName(unsigned int senderSession, const QString &name) {
	// Gated on holding at least one surface, same as before this held names in their own map: a name
	// with nothing to attach to would just accumulate for every session that ever crosses this call,
	// announced stream or not.
	bool hasSurface = false;

	for (auto it = m_surfaces.cbegin(); it != m_surfaces.cend(); ++it) {
		if (it->second.senderSession == senderSession) {
			hasSurface = true;
			break;
		}
	}

	if (!hasSurface) {
		return;
	}

	auto &stored = m_senderNames[senderSession];

	if (stored == name) {
		return;
	}

	stored = name;

	relayout();
}

QString VideoGrid::senderName(unsigned int senderSession) const {
	const auto it = m_senderNames.find(senderSession);

	return it == m_senderNames.end() ? QString() : it->second;
}

void VideoGrid::setStreamCodec(unsigned int senderSession, unsigned int streamID, int sourceKind, int codec) {
	const std::uint64_t key = surfaceKey(senderSession, streamID);
	const bool isNewSurface = m_surfaces.find(key) == m_surfaces.end();
	bool resumeWatching     = false;

	// Stream ids of replaced surfaces this client was still subscribed to - see below.
	std::vector< unsigned int > replacedWatchedStreams;

	if (isNewSurface) {
		bool senderAlreadyPresent = false;

		for (auto it = m_surfaces.cbegin(); it != m_surfaces.cend();) {
			if (it->second.senderSession == senderSession && it->second.sourceKind == sourceKind) {
				// One camera (or screen) per person. A sender restarts a share under a fresh stream id,
				// and if the end of the old one never reached us - the control message was dropped, or
				// the sender's client died - the old surface would sit there as a stuck last frame next
				// to the live one, forever. The newer announcement wins.
				if (it->second.watching) {
					resumeWatching = true;
					replacedWatchedStreams.push_back(it->second.streamID);
				}

				it                   = m_surfaces.erase(it);
				senderAlreadyPresent = true;
				continue;
			}

			if (it->second.senderSession == senderSession) {
				senderAlreadyPresent = true;
			}

			++it;
		}

		// Bounded on distinct senders, not on surfaces held: the cap exists so the grid does not draw
		// more people than a cell size makes worthwhile, and must not stop one person sharing a camera
		// and a screen at once, which is the same sender opening a second stream, not a new one.
		if (!senderAlreadyPresent && distinctSenderCount() >= static_cast< std::size_t >(MAX_SENDERS)) {
			return;
		}

		// The common toggle case: the old stream's end already arrived, so the loop above found nothing
		// to resume from, but the viewer was watching this sender's camera (or screen) moments ago. A
		// restart of the same kind within the grace window resumes watching - see m_recentlyWatched.
		if (!resumeWatching) {
			const auto recent = m_recentlyWatched.find({ senderSession, sourceKind });

			if (recent != m_recentlyWatched.end()) {
				if (QDateTime::currentMSecsSinceEpoch() - recent->second <= RESUME_WATCH_GRACE_MSEC) {
					resumeWatching = true;
				}

				m_recentlyWatched.erase(recent);
			}
		}
	}

	Surface &surface = m_surfaces[key];

	// A codec or source-kind announcement for a stream id already in use replaces it wholesale: a stream
	// id changes whenever the codec, source or dimensions do, so nothing about the old content carries
	// over - least of all a decoder holding reference frames from different content.
	if (surface.streamID != streamID || surface.codec != codec) {
		surface.pendingTiles.clear();
		surface.canvas = QImage();
		surface.vp8.reset();
		surface.tileFrameNumbers.clear();
		surface.lastFrameNumber    = 0;
		surface.hasDecodedFrame    = false;
		surface.awaitingKeyframe   = false;
		surface.unitsWhileAwaiting = 0;
	}

	if (isNewSurface) {
		// Brand new: starts as a greyed-out preview, not auto-watched - the person has to click the
		// eyeball to actually decode it. A surface that already existed and is only transitioning to a
		// new stream id (its sender's resolution or codec changed mid-share) leaves watching exactly as
		// the user last set it instead: someone already watching stays watching through the change,
		// and someone who opted out stays out.
		surface.watching = resumeWatching;
		if (resumeWatching)
			surface.lastUnitMsec = QDateTime::currentMSecsSinceEpoch();
	}

	surface.senderSession = senderSession;
	surface.streamID      = streamID;
	surface.codec         = codec;
	surface.sourceKind    = sourceKind;

	if (isNewSurface) {
		// The watch button must be available before the first frame: an unwatched stream receives none.
		emit senderCountChanged(tileCount());
		relayout();

		// The old stream's surface is gone, but the server was never told to stop relaying it: that is
		// precisely the case this replacement exists for - the old stream's end never arrived - so as far
		// as the server knows this client is still subscribed. Left alone, every silent restart leaks one
		// subscription that keeps costing bandwidth for a picture nothing is painting any more, until the
		// per-user cap is hit and a genuinely wanted stream is refused. Withdrawn before the new one is
		// requested, so the two never race for the same slot under that cap.
		for (const unsigned int oldStreamID : replacedWatchedStreams) {
			emit watchToggled(senderSession, oldStreamID, false);
		}

		if (resumeWatching)
			emit watchToggled(senderSession, streamID, true);
	}
}

void VideoGrid::setWatching(unsigned int senderSession, unsigned int streamID, bool watching) {
	const auto it = m_surfaces.find(surfaceKey(senderSession, streamID));

	if (it == m_surfaces.end() || it->second.watching == watching) {
		return;
	}

	it->second.watching = watching;
	if (!watching) {
		++it->second.pendingUnsubscriptions;
		it->second.pendingTiles.clear();
		it->second.hasDecodedFrame = false;
		it->second.lastFrameNumber = 0;
	}

	if (watching) {
		// A fresh grace period: whatever it was doing while unwatched - possibly nothing, for a long time
		// - must not count against it the instant someone actually starts watching. See
		// checkForStaleStreams().
		it->second.lastUnitMsec          = QDateTime::currentMSecsSinceEpoch();
		it->second.stallRefreshRequested = false;
	}

	if (!watching && m_focus == FocusTarget::Surface && m_focusedSurfaceKey == it->first) {
		// Un-fullscreen whatever was just told to stop being watched, rather than leaving a placeholder
		// filling the whole grid.
		m_focus = FocusTarget::None;
	}

	emit watchToggled(senderSession, streamID, watching);
	relayout();
}

bool VideoGrid::consumeUnsubscribeAcknowledgement(unsigned int senderSession, unsigned int streamID) {
	const auto it = m_surfaces.find(surfaceKey(senderSession, streamID));
	if (it == m_surfaces.end() || it->second.pendingUnsubscriptions == 0) {
		return false;
	}
	--it->second.pendingUnsubscriptions;
	return true;
}

void VideoGrid::onVideoUnitReceived(unsigned int senderSession, unsigned int streamID, quint64 frameNumber,
									bool isKeyframe, unsigned int x, unsigned int y, const QByteArray &encodedTile) {
	// Only streams that announced themselves are decoded. Units arriving for an unannounced stream have
	// no codec, and are dropped rather than assumed to be anything.
	const auto existing = m_surfaces.find(surfaceKey(senderSession, streamID));

	if (existing == m_surfaces.end()) {
		return;
	}

	Surface &surface = existing->second;

	// Recorded regardless of watching state, deliberately - see setWatching() and checkForStaleStreams().
	surface.lastUnitMsec          = QDateTime::currentMSecsSinceEpoch();
	surface.stallRefreshRequested = false;

	if (!surface.watching) {
		// Unwatched: the owner has already sent (or is about to send) a VideoSubscribe withdrawing this,
		// so units are expected to stop arriving shortly, but a few in flight when the button was clicked
		// are ordinary, not a bug. Nothing to paint them into either way - the tile is a placeholder now.
		return;
	}

	// VP8 inter-frames reference their predecessor, and decoding one whose reference is missing does
	// not fail - it returns a plausible-looking corrupted image, classically green. The failure counter
	// below never sees that, so frame continuity is enforced here, before the decoder is fed at all.
	// TiledImage is exempt: its units are independently decodable by design, so late or missing tiles
	// cost nothing beyond the pixels they carried.
	if (surface.codec == MumbleProto::VideoState_Codec_VP8 && surface.hasDecodedFrame) {
		if (frameNumber <= surface.lastFrameNumber) {
			// A stale unit that reassembled after its successors - a keyframe included. Decoding it
			// rewinds the decoder's reference state (and, for a keyframe, would rewind lastFrameNumber
			// itself, making the next live frame look like a gap and triggering a needless freeze).
			return;
		}

		if (surface.awaitingKeyframe && !isKeyframe) {
			// Frozen: the last good picture stays up, which reads as a brief pause instead of a burst
			// of green. Re-ask occasionally in case the first request was lost in the same loss burst
			// that caused the gap.
			if (++surface.unitsWhileAwaiting >= KEYFRAME_REREQUEST_AFTER_UNITS) {
				surface.unitsWhileAwaiting = 0;

				emit keyframeNeeded(senderSession, streamID);
			}

			return;
		}

		if (!isKeyframe && frameNumber != surface.lastFrameNumber + 1) {
			// The reference for this frame never arrived. Freeze and ask once; the counter above
			// repeats the request if the stream keeps flowing without a keyframe.
			surface.awaitingKeyframe   = true;
			surface.unitsWhileAwaiting = 0;

			qWarning("VideoGrid: stream %u/%u lost frame continuity (%llu -> %llu), frozen until a keyframe arrives",
					 senderSession, streamID, static_cast< unsigned long long >(surface.lastFrameNumber),
					 static_cast< unsigned long long >(frameNumber));

			emit keyframeNeeded(senderSession, streamID);

			return;
		}
	}

	if (surface.codec == MumbleProto::VideoState_Codec_TiledImage) {
		// A JPEG tile is a pure function of its own bytes - nothing else about the surface it belongs to
		// matters to decoding it, which is exactly what makes it safe to decode off this thread. Screen
		// sharing is the busiest source of units this class ever handles - a moving or text-heavy share
		// can produce dozens a second - and decoding every one of them synchronously here competed with
		// this thread's own paint and input responsiveness for CPU time. That competition, not anything
		// about layout, was the real, measured source of lag reported on other people's screens and never
		// on the sender's own: a sender's local preview never runs this function at all.
		//
		// VP8 below stays fully synchronous: its decoder carries reference-frame state that must only
		// ever be touched from one thread, in strict frame order, which a background task queue cannot
		// promise the way a stateless JPEG tile can.
		if (m_pendingTileDecodes >= MAX_PENDING_TILE_DECODES
			|| surface.pendingTiles.size() >= MAX_PENDING_TILE_DECODES) {
			emit keyframeNeeded(senderSession, streamID);
			return;
		}
		auto pending         = std::make_shared< PendingTile >();
		pending->frameNumber = frameNumber;
		pending->x           = x;
		pending->y           = y;
		surface.pendingTiles.push_back(pending);
		++m_pendingTileDecodes;
		auto *watcher = new QFutureWatcher< QImage >(this);

		connect(watcher, &QFutureWatcher< QImage >::finished, this,
				[this, watcher, pending, senderSession, streamID]() {
					pending->image = watcher->result();
					pending->ready = true;
					--m_pendingTileDecodes;
					applyReadyTiles(senderSession, streamID);
					watcher->deleteLater();
				});

		watcher->setFuture(QtConcurrent::run(&VideoGrid::decodeJpegTile, encodedTile));

		return;
	}

	const QImage tile = decodeUnit(surface, encodedTile);

	if (tile.isNull()) {
		// Only for a codec this build can actually decode - TiledImage never reaches this line at all any
		// more (dispatched off-thread above), so in practice this now only ever admits VP8, but the check
		// stays explicit rather than assuming that: an unrecognised codec (CODEC_UNKNOWN, or one a future
		// build added that this one predates) fails on every unit forever, and asking its sender for
		// keyframes would be a request nothing can satisfy.
		const bool decodable = surface.codec == MumbleProto::VideoState_Codec_VP8;

		if (decodable && ++surface.consecutiveFailures >= KEYFRAME_REQUEST_AFTER_FAILURES) {
			// Reset on emit, so a sender that ignores the request is asked again only after another full
			// run of failures rather than on every subsequent unit.
			surface.consecutiveFailures = 0;

			emit keyframeNeeded(senderSession, streamID);
		}

		return;
	}

	surface.consecutiveFailures = 0;
	surface.lastFrameNumber     = frameNumber;
	surface.hasDecodedFrame     = true;
	surface.awaitingKeyframe    = false;
	surface.unitsWhileAwaiting  = 0;

	applyDecodedTile(existing->first, surface, x, y, tile);
}

void VideoGrid::applyReadyTiles(unsigned int senderSession, unsigned int streamID) {
	// Workers may finish in any order. Paint in arrival order, or an old JPEG can overwrite a newer
	// one. Re-look up after each paint because UI signals may remove or replace this surface.
	for (;;) {
		const auto it = m_surfaces.find(surfaceKey(senderSession, streamID));
		if (it == m_surfaces.end() || it->second.pendingTiles.empty() || !it->second.pendingTiles.front()->ready) {
			return;
		}
		const std::shared_ptr< PendingTile > tile = it->second.pendingTiles.front();
		it->second.pendingTiles.pop_front();
		onTiledImageTileDecoded(senderSession, streamID, tile->x, tile->y, tile->image, tile->frameNumber);
	}
}

void VideoGrid::onTiledImageTileDecoded(unsigned int senderSession, unsigned int streamID, unsigned int x,
										unsigned int y, QImage tile, quint64 frameNumber) {
	// Re-looked-up rather than captured by reference at dispatch time: this decode ran on a background
	// thread, so by the time it finishes, the stream may have ended, or the surface holding it destroyed
	// entirely, while it was in flight.
	const auto existing = m_surfaces.find(surfaceKey(senderSession, streamID));

	if (existing == m_surfaces.end()) {
		return;
	}

	Surface &surface = existing->second;

	if (!surface.watching || surface.codec != MumbleProto::VideoState_Codec_TiledImage) {
		// Either unwatched since this decode was dispatched, or the stream was re-announced under a
		// different codec in the meantime - either way, a stale result that must be dropped rather than
		// painted into a surface it no longer describes.
		return;
	}

	if (tile.isNull()) {
		// Only for a codec this build can decode - which TiledImage always is, so no such gate is needed
		// here the way onVideoUnitReceived()'s own synchronous path needs one for an announced-but-unknown
		// codec.
		if (++surface.consecutiveFailures >= KEYFRAME_REQUEST_AFTER_FAILURES) {
			surface.consecutiveFailures = 0;

			emit keyframeNeeded(senderSession, streamID);
		}

		return;
	}

	// Reassembly order can differ from network frame order. Dropping a late tile may leave a gap until
	// refresh, but it must never overwrite newer pixels. Applied after decoding so invalid JPEGs cannot
	// move it forward.
	//
	// The watermark is per tile position, not per surface. TiledImage only ever sends the tiles that
	// changed, so frame N+1 typically carries a handful of tiles and frame N's stragglers cover other
	// parts of the screen entirely: a surface-wide watermark threw those away the moment any tile of
	// N+1 landed first, and a region that had never been painted before stayed a black rectangle until
	// the periodic refresh got round to it. Only a tile that a newer frame has actually painted at the
	// same spot has anything to protect.
	const auto tileKey = (static_cast< std::uint64_t >(x) << 32) | y;
	const auto painted = surface.tileFrameNumbers.find(tileKey);

	if (painted != surface.tileFrameNumbers.end() && frameNumber < painted->second) {
		return;
	}

	surface.tileFrameNumbers[tileKey] = frameNumber;
	surface.hasDecodedFrame           = true;
	surface.lastFrameNumber           = std::max(surface.lastFrameNumber, frameNumber);
	surface.consecutiveFailures       = 0;

	applyDecodedTile(existing->first, surface, x, y, tile);
}

void VideoGrid::applyDecodedTile(std::uint64_t key, Surface &surface, unsigned int x, unsigned int y,
								 const QImage &tile) {
	// A surface exists from the announcement onwards but holds no picture until now, so this is what
	// makes the sender count - and with it the video panel - appear.
	const bool wasBlank = surface.canvas.isNull();

	if (!growToFit(surface.canvas, static_cast< int >(x), static_cast< int >(y), tile.width(), tile.height())) {
		return;
	}

	{
		QPainter painter(&surface.canvas);
		painter.drawImage(static_cast< int >(x), static_cast< int >(y), tile);
	}

	// A brand new picture changes which cells are occupied and what every control bar's geometry should
	// be, so it needs the full relayout(). A tile updating within a surface that already had a picture
	// changes none of that - only the pixels themselves, and, if this happens to be the surface currently
	// shown full-screen, that window's content too. relayoutControls() does real per-tile widget work
	// (setGeometry(), raise(), sometimes creating a QWidget) every time it runs; paying that cost for
	// every other participant's tile on every single incoming unit - which for a busy screen share or a
	// moving camera can be dozens a second - was a real, measurable source of lag for anyone watching more
	// than a tile or two, without the sender who never triggers this path ever seeing it. update() alone
	// is enough for the picture itself: Qt coalesces any number of update() calls made before the next
	// paint into one repaint, unlike relayoutControls()'s immediate, uncoalesced widget work.
	if (wasBlank) {
		emit senderCountChanged(tileCount());
		relayout();

		return;
	}

	if (m_focus == FocusTarget::Surface && m_focusedSurfaceKey == key) {
		// Just the rectangle this tile covers, in canvas pixels: the fullscreen window maps that onto the
		// monitor and repaints only that patch. Repainting the whole monitor for every 128-pixel tile of
		// a screen share was, alongside the canvas copy described on FullscreenVideoWindow, what made
		// watching one fullscreen so expensive.
		notifyFullscreenContentChanged(QRect(static_cast< int >(x), static_cast< int >(y), tile.width(), tile.height()));
	}

	// Likewise only this tile's own cell here, not the whole grid: paintEvent() is clipped to the region
	// it is asked for, so every other tile's scaling is skipped rather than redone on each incoming unit.
	const int slot = slotForSurface(key);

	if (slot >= 0) {
		update(cellRect(currentLayout(), slot));
	} else {
		update();
	}
}

void VideoGrid::setSelfCameraFrame(const QImage &frame) {
	const bool wasEmpty = m_selfCameraFrame.isNull();

	m_selfCameraFrame = frame;

	// See the equivalent comment in onVideoUnitReceived(): a continuing preview updating its picture does
	// not change the layout, only appearing or disappearing does.
	if (wasEmpty != frame.isNull()) {
		emit senderCountChanged(tileCount());
		relayout();

		return;
	}

	if (m_focus == FocusTarget::SelfCamera) {
		notifyFullscreenContentChanged(frame.rect());
	}

	// Own camera is always the first cell, when present.
	update(cellRect(currentLayout(), 0));
}

void VideoGrid::clearSelfCameraFrame() {
	if (m_selfCameraFrame.isNull()) {
		return;
	}

	m_selfCameraFrame = QImage();

	emit senderCountChanged(tileCount());
	relayout();
}

void VideoGrid::setSelfScreenFrame(const QImage &frame) {
	const bool wasEmpty = m_selfScreenFrame.isNull();

	m_selfScreenFrame = frame;

	if (wasEmpty != frame.isNull()) {
		emit senderCountChanged(tileCount());
		relayout();

		return;
	}

	if (m_focus == FocusTarget::SelfScreen) {
		notifyFullscreenContentChanged(frame.rect());
	}

	// Own screen sits after own camera, if there is one.
	update(cellRect(currentLayout(), m_selfCameraFrame.isNull() ? 0 : 1));
}

void VideoGrid::clearSelfScreenFrame() {
	if (m_selfScreenFrame.isNull()) {
		return;
	}

	m_selfScreenFrame = QImage();

	emit senderCountChanged(tileCount());
	relayout();
}

void VideoGrid::removeSender(unsigned int senderSession, unsigned int streamID) {
	const auto it = m_surfaces.find(surfaceKey(senderSession, streamID));

	if (it == m_surfaces.end()) {
		return;
	}

	// Remember a watched stream's sender+kind so a restart within the grace window resumes watching
	// rather than dropping the viewer back to a placeholder - see m_recentlyWatched.
	if (it->second.watching) {
		m_recentlyWatched[{ senderSession, it->second.sourceKind }] = QDateTime::currentMSecsSinceEpoch();
	}

	m_surfaces.erase(it);
	emit senderCountChanged(tileCount());
	relayout();
}

void VideoGrid::removeSender(unsigned int senderSession) {
	bool removedAny = false;

	for (auto it = m_surfaces.begin(); it != m_surfaces.end();) {
		if (it->second.senderSession == senderSession) {
			it         = m_surfaces.erase(it);
			removedAny = true;
		} else {
			++it;
		}
	}

	m_senderNames.erase(senderSession);

	// The sender is gone entirely - do not resume watching them if some later stream reuses the session.
	for (auto it = m_recentlyWatched.begin(); it != m_recentlyWatched.end();) {
		it = (it->first.first == senderSession) ? m_recentlyWatched.erase(it) : std::next(it);
	}

	if (removedAny) {
		emit senderCountChanged(tileCount());
		relayout();
	}
}

void VideoGrid::checkForStaleStreams() {
	const qint64 now = QDateTime::currentMSecsSinceEpoch();

	// Collected first, rather than erased while walking m_surfaces directly: removeSender() below mutates
	// the very map this loop would otherwise still be iterating.
	std::vector< std::pair< unsigned int, unsigned int > > stale;

	for (auto &entry : m_surfaces) {
		Surface &surface = entry.second;

		if (!surface.watching) {
			continue;
		}

		const qint64 silentFor = now - surface.lastUnitMsec;

		if (silentFor > m_staleStreamTimeoutMsec) {
			stale.emplace_back(surface.senderSession, surface.streamID);

			continue;
		}

		// A lighter, faster sibling of the stale-drop check above: a stream that is merely overdue for its
		// next update - a lost periodic refresh, a burst of loss the codec's own recovery has not caught
		// up with yet - gets an explicit nudge long before it would ever be given up on entirely. TiledImage
		// has no other proactive recovery signal at all (unlike VP8's own frame-continuity tracking further
		// up this file), so this is the only thing that can shorten its recovery time below waiting out the
		// periodic refresh cycle. Asked once per stall, not once per poll: stallRefreshRequested is what
		// stops this from re-firing every poll interval for the same silence.
		if (silentFor > m_stallRefreshTimeoutMsec && !surface.stallRefreshRequested) {
			surface.stallRefreshRequested = true;

			emit keyframeNeeded(surface.senderSession, surface.streamID);
		}
	}

	for (const auto &[senderSession, streamID] : stale) {
		removeSender(senderSession, streamID);

		// After removal, not before: the doc comment on this signal promises the surface is already gone
		// by the time anything sees it, and the owner's response (withdrawing the subscription) has
		// nothing useful left to race against on this side either way.
		emit streamWentStale(senderSession, streamID);
	}
}

void VideoGrid::clear() {
	m_focus             = FocusTarget::None;
	m_focusedSurfaceKey = 0;

	m_ownCameraControls.reset();
	m_ownScreenControls.reset();
	m_remoteControls.clear();

	// Every bar this grid had is gone, so whatever updateHoveredBarImpl() last decided about them cannot
	// be reused - see m_controlsGeneration. Also reset the hovered slot outright: nothing is hovered when
	// there is nothing left to hover.
	++m_controlsGeneration;
	m_hoveredSlot = -1;

	const bool hadAnything = !m_surfaces.empty() || !m_selfCameraFrame.isNull() || !m_selfScreenFrame.isNull();

	m_surfaces.clear();
	m_senderNames.clear();
	m_selfCameraFrame = QImage();
	m_selfScreenFrame = QImage();

	if (!hadAnything) {
		return;
	}

	emit senderCountChanged(0);
	relayout();
}

void VideoGrid::clearFocusedTile() {
	if (m_focus == FocusTarget::None) {
		return;
	}

	m_focus = FocusTarget::None;
	relayout();
}

VideoGrid::Layout VideoGrid::currentLayout() const {
	Layout layout;
	layout.count = tileCount();

	if (layout.count == 0) {
		return layout;
	}

	// A roughly square arrangement, which is what every other video call looks like and needs no layout
	// configuration to be reasonable at any participant count.
	layout.columns = static_cast< int >(std::ceil(std::sqrt(static_cast< double >(layout.count))));
	layout.rows    = (layout.count + layout.columns - 1) / layout.columns;

	layout.cellWidth  = width() / layout.columns;
	layout.cellHeight = height() / layout.rows;

	return layout;
}

QRect VideoGrid::cellRect(const Layout &layout, int slot) {
	if (layout.columns <= 0) {
		return QRect();
	}

	const int column = slot % layout.columns;
	const int row    = slot / layout.columns;

	return QRect(column * layout.cellWidth, row * layout.cellHeight, layout.cellWidth, layout.cellHeight);
}

int VideoGrid::slotAt(const QPoint &point, const Layout &layout) const {
	if (layout.count == 0 || layout.cellWidth <= 0 || layout.cellHeight <= 0) {
		return -1;
	}

	const int column = point.x() / layout.cellWidth;
	const int row    = point.y() / layout.cellHeight;

	if (column < 0 || column >= layout.columns || row < 0 || row >= layout.rows) {
		return -1;
	}

	const int slot = row * layout.columns + column;

	// The last row may be short of a full set of columns - a count that is not a perfect square always
	// leaves some cells in the grid unoccupied, and a click that lands in one of those must not be
	// mistaken for the tile before it.
	return slot < layout.count ? slot : -1;
}

std::unique_ptr< VideoGrid::TileControlBar > VideoGrid::makeOwnControlBar(FocusTarget target) {
	auto controls = std::make_unique< TileControlBar >();

	controls->bar = new QWidget(this);
	controls->bar->setStyleSheet(QStringLiteral("background-color: rgba(0, 0, 0, 140);"));

	auto *layout = new QHBoxLayout(controls->bar);
	layout->setContentsMargins(4, 2, 4, 2);
	layout->setSpacing(4);
	layout->addStretch();

	controls->fullscreenButton = new QToolButton(controls->bar);
	controls->fullscreenButton->setAutoRaise(true);
	controls->fullscreenButton->setText(QStringLiteral("⛶"));
	controls->fullscreenButton->setToolTip(tr("Fullscreen"));
	layout->addWidget(controls->fullscreenButton);

	connect(controls->fullscreenButton, &QToolButton::clicked, this, [this, target]() {
		if (m_focus == target) {
			clearFocusedTile();
		} else {
			m_focus = target;
			relayout();
		}
	});

	controls->bar->show();

	return controls;
}

std::unique_ptr< VideoGrid::TileControlBar > VideoGrid::makeRemoteControlBar(unsigned int senderSession,
																			 unsigned int streamID, bool hasAudio) {
	auto controls = std::make_unique< TileControlBar >();

	controls->bar = new QWidget(this);
	controls->bar->setStyleSheet(QStringLiteral("background-color: rgba(0, 0, 0, 140);"));

	auto *layout = new QHBoxLayout(controls->bar);
	layout->setContentsMargins(4, 2, 4, 2);
	layout->setSpacing(4);

	// A camera has no audio of its own to control - voice is muted and adjusted the same way it always
	// has been, from the user list. Only a screen share carries a second, independent audio stream worth
	// a slider of its own.
	if (hasAudio) {
		controls->volumeSlider = new QSlider(Qt::Horizontal, controls->bar);
		controls->volumeSlider->setRange(0, 100);
		controls->volumeSlider->setValue(100);
		controls->volumeSlider->setFixedWidth(80);
		controls->volumeSlider->setToolTip(tr("Volume"));
		layout->addWidget(controls->volumeSlider);

		connect(controls->volumeSlider, &QSlider::valueChanged, this, [this, senderSession](int value) {
			emit volumeChanged(senderSession, static_cast< float >(value) / 100.0f);
		});
	}

	layout->addStretch();

	controls->fullscreenButton = new QToolButton(controls->bar);
	controls->fullscreenButton->setAutoRaise(true);
	controls->fullscreenButton->setText(QStringLiteral("⛶"));
	controls->fullscreenButton->setToolTip(tr("Fullscreen"));
	layout->addWidget(controls->fullscreenButton);

	const std::uint64_t key = surfaceKey(senderSession, streamID);

	connect(controls->fullscreenButton, &QToolButton::clicked, this, [this, key]() {
		if (m_focus == FocusTarget::Surface && m_focusedSurfaceKey == key) {
			clearFocusedTile();
		} else {
			m_focus             = FocusTarget::Surface;
			m_focusedSurfaceKey = key;
			relayout();
		}
	});

	controls->watchButton = new QToolButton(controls->bar);
	controls->watchButton->setAutoRaise(true);
	layout->addWidget(controls->watchButton);

	connect(controls->watchButton, &QToolButton::clicked, this, [this, senderSession, streamID, key]() {
		const auto it = m_surfaces.find(key);

		if (it == m_surfaces.end()) {
			return;
		}

		setWatching(senderSession, streamID, !it->second.watching);
	});

	controls->bar->show();

	return controls;
}

void VideoGrid::relayoutControls(const Layout &layout) {
	++m_relayoutControlsCallCount;

	constexpr int BAR_HEIGHT = 28;

	// The grid always shows its normal multi-tile layout now, fullscreen or not - see the class comment
	// on FullscreenVideoWindow for why a focused tile no longer makes paintEvent() fill the whole widget
	// with just that one picture. So there is only ever one layout to position bars against here.

	int index = 0;

	// Every create and destroy below bumps m_controlsGeneration - see updateHoveredBarImpl(), which uses it
	// to tell "the layout moved" (nothing to redo) from "the set of bars changed" (everything to redo).
	// Only real changes count: a reset() of something already null, or a find() that was already there, is
	// not a change and must not invalidate anything.
	if (!m_selfCameraFrame.isNull()) {
		if (!m_ownCameraControls) {
			m_ownCameraControls = makeOwnControlBar(FocusTarget::SelfCamera);
			++m_controlsGeneration;
		}

		const QRect cell = cellRect(layout, index++);
		m_ownCameraControls->bar->setGeometry(cell.x(), cell.bottom() - BAR_HEIGHT + 1, cell.width(), BAR_HEIGHT);
		m_ownCameraControls->bar->raise();
	} else if (m_ownCameraControls) {
		m_ownCameraControls.reset();
		++m_controlsGeneration;
	}

	if (!m_selfScreenFrame.isNull()) {
		if (!m_ownScreenControls) {
			m_ownScreenControls = makeOwnControlBar(FocusTarget::SelfScreen);
			++m_controlsGeneration;
		}

		const QRect cell = cellRect(layout, index++);
		m_ownScreenControls->bar->setGeometry(cell.x(), cell.bottom() - BAR_HEIGHT + 1, cell.width(), BAR_HEIGHT);
		m_ownScreenControls->bar->raise();
	} else if (m_ownScreenControls) {
		m_ownScreenControls.reset();
		++m_controlsGeneration;
	}

	// Bars for a sender that no longer holds a surface at all are dropped outright; bars for a surface
	// that is merely unwatched are kept, since the watch button is exactly how a placeholder tile gets
	// out of that state.
	for (auto it = m_remoteControls.begin(); it != m_remoteControls.end();) {
		if (m_surfaces.find(it->first) == m_surfaces.end()) {
			it = m_remoteControls.erase(it);
			++m_controlsGeneration;
		} else {
			++it;
		}
	}

	for (auto &entry : m_surfaces) {
		const Surface &surface = entry.second;

		// Unwatched-but-blank is exactly the preview state, not a slot to skip - it is watched-but-blank
		// (announced, watching just requested, first frame not decoded yet) that is the brief race worth
		// skipping a bar for.
		if (surface.watching && surface.canvas.isNull()) {
			continue;
		}

		auto controlsIt = m_remoteControls.find(entry.first);

		if (controlsIt == m_remoteControls.end()) {
			const bool hasAudio = surface.sourceKind != MumbleProto::VideoState_SourceKind_Camera;
			controlsIt =
				m_remoteControls
					.emplace(entry.first, makeRemoteControlBar(surface.senderSession, surface.streamID, hasAudio))
					.first;
			++m_controlsGeneration;
		}

		TileControlBar &controls = *controlsIt->second;

		if (controls.watchButton) {
			controls.watchButton->setText(surface.watching ? QStringLiteral("✕") : QStringLiteral("👁"));
			controls.watchButton->setToolTip(surface.watching ? tr("Stop watching") : tr("Watch"));
		}

		const QRect cell = cellRect(layout, index++);
		controls.bar->setGeometry(cell.x(), cell.bottom() - BAR_HEIGHT + 1, cell.width(), BAR_HEIGHT);
		controls.bar->raise();
	}
}

void VideoGrid::updateHoveredBar() {
	if (m_updatingHoveredBar) {
		// Already running, somewhere further down this same call stack - see m_updatingHoveredBar. That
		// pass is still working from the same m_mouseInside/m_lastMousePos this one would have used, so
		// skipping the nested call loses nothing, exactly as for m_relayoutInProgress.
		return;
	}

	m_updatingHoveredBar = true;
	updateHoveredBarImpl();
	m_updatingHoveredBar = false;
}

void VideoGrid::updateHoveredBarImpl() {
	// Worked out before anything is touched, so that the overwhelmingly common case - this being called
	// again with the hover state exactly as it already is - can return without changing any widget's
	// visibility at all.
	//
	// That case is not rare, it is most of them: relayout() calls this, and resizeEvent() calls relayout()
	// unconditionally, so dragging a splitter beside the video panel runs this on every resize tick, dozens
	// a second. Hiding every bar and re-showing one of them on each of those is pure churn - the hovered
	// tile has not changed, only the geometry has - and it is churn of the exact kind that has now produced
	// two separate crashes in this class: hide/show work colliding with Qt machinery that is busy doing
	// something else with the same widgets. Not doing the work is a better fix than doing it more carefully.
	const int slot = m_mouseInside ? slotAt(m_lastMousePos, currentLayout()) : -1;

	// The generation is what keeps this honest. A slot index is positional, not an identity: after a tile
	// appears or disappears, the same index can be a different sender's tile, and returning early on the
	// index alone would leave the wrong bar - or a bar belonging to a stream that just ended - showing.
	// relayoutControls() bumps the generation whenever it creates or destroys one, so any structural change
	// forces the full pass below; a plain resize, which only moves bars, does not.
	if (slot == m_hoveredSlot && m_controlsGeneration == m_hoveredBarGeneration) {
		return;
	}

	m_hoveredSlot          = slot;
	m_hoveredBarGeneration = m_controlsGeneration;

	++m_hoveredBarAppliedCount;

	// Hidden first, unconditionally - simpler than tracking which individual bar was visible last time,
	// and now that the check above means this only runs when something actually changed, the cost of the
	// extra hides is paid on hover changes rather than on every resize tick.
	if (m_ownCameraControls) {
		m_ownCameraControls->bar->setVisible(false);
	}

	if (m_ownScreenControls) {
		m_ownScreenControls->bar->setVisible(false);
	}

	for (auto &entry : m_remoteControls) {
		entry.second->bar->setVisible(false);
	}

	if (slot < 0) {
		return;
	}

	// Same walk order as paintEvent(), mouseDoubleClickEvent() and relayoutControls() - see the latter's
	// comment on why m_surfaces being a std::map is what makes this order something worth relying on.
	int index = 0;

	if (!m_selfCameraFrame.isNull()) {
		if (index == slot) {
			if (m_ownCameraControls) {
				m_ownCameraControls->bar->setVisible(true);
			}

			return;
		}

		++index;
	}

	if (!m_selfScreenFrame.isNull()) {
		if (index == slot) {
			if (m_ownScreenControls) {
				m_ownScreenControls->bar->setVisible(true);
			}

			return;
		}

		++index;
	}

	for (auto &entry : m_surfaces) {
		if (entry.second.watching && entry.second.canvas.isNull()) {
			continue;
		}

		if (index == slot) {
			const auto it = m_remoteControls.find(entry.first);

			if (it != m_remoteControls.end()) {
				it->second->bar->setVisible(true);
			}

			return;
		}

		++index;
	}
}

void VideoGrid::updateFullscreenWindow() {
	if (m_focus == FocusTarget::None) {
		if (m_fullscreenWindow) {
			m_fullscreenWindow->hide();
		}

		return;
	}

	// validateFocus() has already run by the time relayout() gets here, so whatever is focused has a
	// picture - but the window is created lazily and reads that picture for itself, at paint time, rather
	// than being handed a copy. See FullscreenVideoWindow's class comment for why the copy was the whole
	// problem.
	if (!m_fullscreenWindow) {
		m_fullscreenWindow = new FullscreenVideoWindow(this, [this](QImage &image, QString &label) {
			if (m_focus == FocusTarget::SelfCamera) {
				image = m_selfCameraFrame;
				label = tr("You");
			} else if (m_focus == FocusTarget::SelfScreen) {
				image = m_selfScreenFrame;
				label = tr("You (screen)");
			} else if (m_focus == FocusTarget::Surface) {
				const auto it = m_surfaces.find(m_focusedSurfaceKey);

				if (it != m_surfaces.end()) {
					image = it->second.canvas;
					label = labelForSurface(it->second);
				}
			}
		});
	}

	// A relayout() is the "something structural changed" path - a different tile focused, a stream
	// re-announced, the picture appearing for the first time - so the whole window is repainted. The
	// per-tile path, applyDecodedTile(), goes through notifyFullscreenContentChanged() with just the
	// changed rectangle instead.
	static_cast< FullscreenVideoWindow * >(m_fullscreenWindow)->contentReplaced();

	if (!m_fullscreenWindow->isVisible()) {
		m_fullscreenWindow->showFullScreen();
	}
}

void VideoGrid::notifyFullscreenContentChanged(const QRect &changed) {
	if (!m_fullscreenWindow || m_focus == FocusTarget::None) {
		return;
	}

	static_cast< FullscreenVideoWindow * >(m_fullscreenWindow)->contentChanged(changed);
}

int VideoGrid::slotForSurface(std::uint64_t key) const {
	// Same walk order as paintEvent(), relayoutControls(), updateHoveredBar() and the mouse handlers.
	int index = (m_selfCameraFrame.isNull() ? 0 : 1) + (m_selfScreenFrame.isNull() ? 0 : 1);

	for (auto it = m_surfaces.cbegin(); it != m_surfaces.cend(); ++it) {
		if (it->second.watching && it->second.canvas.isNull()) {
			continue;
		}

		if (it->first == key) {
			return index;
		}

		++index;
	}

	return -1;
}

QString VideoGrid::labelForSurface(const Surface &surface) const {
	const auto nameIt = m_senderNames.find(surface.senderSession);
	QString label     = nameIt == m_senderNames.end() ? QString() : nameIt->second;

	// A camera tile is unlabelled beyond the name, matching today's behaviour. Anything else - a
	// screen or a window - is called out, since a sender showing a camera and a screen at once
	// would otherwise present as one person with two identical names.
	switch (surface.sourceKind) {
		case MumbleProto::VideoState_SourceKind_Display:
			return label.isEmpty() ? tr("Screen") : tr("%1 (screen)").arg(label);
		case MumbleProto::VideoState_SourceKind_Window:
			return label.isEmpty() ? tr("Window") : tr("%1 (window)").arg(label);
		case MumbleProto::VideoState_SourceKind_Application:
			return label.isEmpty() ? tr("App") : tr("%1 (app)").arg(label);
		default:
			return label;
	}
}

void VideoGrid::validateFocus() {
	// A stream can end, or be unwatched from its own control bar, while it fills the grid - and unlike a
	// closed remote stream, a self stream cannot even leave a placeholder behind, since it has no watch
	// button to bring it back with.
	if (m_focus == FocusTarget::SelfCamera && m_selfCameraFrame.isNull()) {
		m_focus = FocusTarget::None;
	} else if (m_focus == FocusTarget::SelfScreen && m_selfScreenFrame.isNull()) {
		m_focus = FocusTarget::None;
	} else if (m_focus == FocusTarget::Surface) {
		const auto it = m_surfaces.find(m_focusedSurfaceKey);

		if (it == m_surfaces.end() || it->second.canvas.isNull() || !it->second.watching) {
			m_focus = FocusTarget::None;
		}
	}
}

void VideoGrid::relayout() {
	if (m_relayoutInProgress) {
		// Already running, somewhere further down this same call stack - see m_relayoutInProgress. That
		// pass will see whatever change asked for this one by the time it gets there.
		return;
	}

	m_relayoutInProgress = true;

	validateFocus();
	relayoutControls(currentLayout());
	updateHoveredBar();
	updateFullscreenWindow();
	update();

	m_relayoutInProgress = false;
}

void VideoGrid::resizeEvent(QResizeEvent *event) {
	QWidget::resizeEvent(event);

	// A plain resize does not change which tile is in which cell, only where the cells themselves are -
	// but that is exactly what a control bar's geometry has to track, so it needs the same relayout a
	// tile actually appearing or disappearing does.
	relayout();
}

void VideoGrid::paintEvent(QPaintEvent *) {
	QPainter painter(this);
	painter.fillRect(rect(), Qt::black);

	// Scaled to fit inside its cell without distorting it. Letterboxing is the honest presentation:
	// stretching somebody's screen share to fill a cell makes text unreadable.
	const auto drawInto = [&](const QImage &image, const QRect &cell, const QString &label) {
		const QSize scaled = image.size().scaled(cell.size(), Qt::KeepAspectRatio);

		const QRect target(cell.x() + (cell.width() - scaled.width()) / 2,
						   cell.y() + (cell.height() - scaled.height()) / 2, scaled.width(), scaled.height());

		painter.drawImage(target, image);

		if (!label.isEmpty()) {
			// Drawn inside the picture rather than the cell. A letterboxed tile leaves black margins, and
			// a name sitting out in one of those reads as belonging to nothing in particular.
			const QRect labelRect = target.adjusted(4, 4, -4, -4);

			// A dark strip behind it, because white text over a bright frame is unreadable and the frame
			// is somebody's camera - its brightness is not ours to predict.
			QRect backdrop = painter.fontMetrics().boundingRect(labelRect, Qt::AlignTop | Qt::AlignLeft, label);
			backdrop.adjust(-3, -1, 3, 1);

			painter.fillRect(backdrop, QColor(0, 0, 0, 140));

			painter.setPen(Qt::white);
			painter.drawText(labelRect, Qt::AlignTop | Qt::AlignLeft, label);
		}
	};

	// A placeholder for a tile whose stream is not currently being watched - dark, labelled, and with
	// nothing decoded into it drawn, so it is obvious at a glance that this is a deliberate "not watching"
	// state rather than a stalled or broken picture.
	//
	// The eyeball is drawn large and dead centre, as the button it now actually is: a single click
	// anywhere on the placeholder starts watching (see mouseReleaseEvent()), so the picture should look
	// like something to click rather than a caption. The name sits above it in the ordinary text size.
	const auto drawPlaceholder = [&](const QRect &cell, const QString &label) {
		painter.fillRect(cell, QColor(32, 32, 32));

		const QFont normalFont = painter.font();
		QFont eyeFont          = normalFont;
		eyeFont.setPointSize(std::max(normalFont.pointSize() * 2, std::min(cell.width(), cell.height()) / 5));

		painter.setFont(eyeFont);
		painter.setPen(QColor(210, 210, 210));
		painter.drawText(cell, Qt::AlignCenter, QStringLiteral("👁"));

		const int eyeHeight = painter.fontMetrics().height();
		painter.setFont(normalFont);
		painter.setPen(QColor(180, 180, 180));

		// Name above the eye, caption below it - each in the half of the cell the eye is not in.
		const QRect above(cell.x(), cell.y(), cell.width(), (cell.height() - eyeHeight) / 2);
		const QRect below(cell.x(), cell.y() + (cell.height() + eyeHeight) / 2, cell.width(),
						  (cell.height() - eyeHeight) / 2);

		if (!label.isEmpty()) {
			painter.drawText(above, Qt::AlignHCenter | Qt::AlignBottom | Qt::TextWordWrap, label);
		}

		painter.drawText(below, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, tr("Click to watch"));
	};

	// The grid always shows every tile in its own cell now, fullscreen or not - a fullscreened tile is
	// shown full-screen in FullscreenVideoWindow, a genuine top-level window, not by VideoGrid filling
	// its own (dock-sized) rect with just that one picture the way an early version did.

	// Tested on what is drawable, not on whether any surface exists. A surface is created when a stream
	// is announced and stays blank until the first frame decodes, so "holds surfaces" and "has something
	// to draw" are different questions; conflating them let a blank surface reach the layout below with
	// a count of zero, and divide by it.
	const Layout layout = currentLayout();

	if (layout.count == 0 || layout.cellWidth <= 0 || layout.cellHeight <= 0) {
		return;
	}

	int index = 0;

	if (!m_selfCameraFrame.isNull()) {
		drawInto(m_selfCameraFrame, cellRect(layout, index++), tr("You"));
	}

	if (!m_selfScreenFrame.isNull()) {
		drawInto(m_selfScreenFrame, cellRect(layout, index++), tr("You (screen)"));
	}

	for (auto it = m_surfaces.cbegin(); it != m_surfaces.cend(); ++it) {
		const Surface &surface = it->second;

		if (surface.watching && surface.canvas.isNull()) {
			continue;
		}

		const QRect cell = cellRect(layout, index++);

		if (surface.watching) {
			drawInto(surface.canvas, cell, labelForSurface(surface));
		} else {
			drawPlaceholder(cell, labelForSurface(surface));
		}
	}
}

const VideoGrid::Surface *VideoGrid::surfaceAtSlot(int slot) const {
	if (slot < 0) {
		return nullptr;
	}

	// Same walk order as paintEvent(): own camera, own screen, then m_surfaces in key order.
	int index = (m_selfCameraFrame.isNull() ? 0 : 1) + (m_selfScreenFrame.isNull() ? 0 : 1);

	if (slot < index) {
		return nullptr;
	}

	for (auto it = m_surfaces.cbegin(); it != m_surfaces.cend(); ++it) {
		if (it->second.watching && it->second.canvas.isNull()) {
			continue;
		}

		if (index == slot) {
			return &it->second;
		}

		++index;
	}

	return nullptr;
}

void VideoGrid::mousePressEvent(QMouseEvent *event) {
	m_pressedSlot = event->button() == Qt::LeftButton ? slotAt(event->pos(), currentLayout()) : -1;

	if (m_pressedSlot >= 0) {
		event->accept();

		return;
	}

	QWidget::mousePressEvent(event);
}

void VideoGrid::mouseReleaseEvent(QMouseEvent *event) {
	const int pressedSlot = m_pressedSlot;
	m_pressedSlot         = -1;

	// A click is a press and a release on the same tile - a drag that ends somewhere else is not one.
	if (event->button() != Qt::LeftButton || pressedSlot < 0 || slotAt(event->pos(), currentLayout()) != pressedSlot) {
		QWidget::mouseReleaseEvent(event);

		return;
	}

	const Surface *surface = surfaceAtSlot(pressedSlot);

	if (!surface || surface->watching) {
		// Watched tiles, and your own, act on double-click (fullscreen) and on their control bar, not on
		// a plain click - a single click on a live picture doing something would make it far too easy to
		// fullscreen a tile by accident while reaching for its volume slider.
		QWidget::mouseReleaseEvent(event);

		return;
	}

	// The placeholder is the button: "click to watch" means exactly that, not "find the small eyeball
	// in the hover bar at the bottom right of this tile and click that". The bar's eyeball still works
	// too, and is what stops watching again.
	m_lastClickWatchMsec = QDateTime::currentMSecsSinceEpoch();
	setWatching(surface->senderSession, surface->streamID, true);

	event->accept();
}

void VideoGrid::mouseDoubleClickEvent(QMouseEvent *event) {
	// A double-click is delivered as press, release, double-click, release - so by the time it arrives,
	// the release before it has already started watching the placeholder that was clicked, and that
	// placeholder has already left the layout (a watched-but-blank stream holds no cell until its first
	// frame lands). Whatever tile reflowed into that slot is not what the user meant to fullscreen.
	if (m_lastClickWatchMsec != 0
		&& QDateTime::currentMSecsSinceEpoch() - m_lastClickWatchMsec <= QApplication::doubleClickInterval()) {
		event->accept();

		return;
	}

	const Layout layout = currentLayout();
	const int slot      = slotAt(event->pos(), layout);

	if (slot < 0) {
		event->ignore();

		return;
	}

	// Walked in the same order paintEvent() draws in - own camera, own screen, then m_surfaces in order -
	// so the slot a click landed on and the tile it visually looked like it landed on are always the same
	// tile. m_surfaces being a std::map, not an unordered_map, is exactly what makes that order something
	// a click can rely on between one repaint and the next.
	//
	// Double-clicking whichever tile is already fullscreened is what un-fullscreens it - the grid always
	// shows its normal layout, fullscreen or not, so there is no "any click anywhere shrinks back" case
	// distinct from clicking the specific tile that is focused.
	int index = 0;

	if (!m_selfCameraFrame.isNull()) {
		if (index == slot) {
			if (m_focus == FocusTarget::SelfCamera) {
				clearFocusedTile();
			} else {
				m_focus = FocusTarget::SelfCamera;
				// So Esc works immediately, without the user having to click the grid a second time just
				// to give it keyboard focus.
				setFocus(Qt::MouseFocusReason);
				relayout();
			}

			event->accept();

			return;
		}

		++index;
	}

	if (!m_selfScreenFrame.isNull()) {
		if (index == slot) {
			if (m_focus == FocusTarget::SelfScreen) {
				clearFocusedTile();
			} else {
				m_focus = FocusTarget::SelfScreen;
				setFocus(Qt::MouseFocusReason);
				relayout();
			}

			event->accept();

			return;
		}

		++index;
	}

	for (auto it = m_surfaces.cbegin(); it != m_surfaces.cend(); ++it) {
		if (it->second.watching && it->second.canvas.isNull()) {
			continue;
		}

		if (index == slot) {
			if (!it->second.watching) {
				// Double-clicking a preview tile is the same as clicking its eyeball - it is the obvious
				// thing to try first, and there is no useful "fullscreen a placeholder" behaviour to give
				// up in exchange for supporting it.
				setWatching(it->second.senderSession, it->second.streamID, true);

				event->accept();

				return;
			}

			if (m_focus == FocusTarget::Surface && m_focusedSurfaceKey == it->first) {
				clearFocusedTile();
			} else {
				m_focus             = FocusTarget::Surface;
				m_focusedSurfaceKey = it->first;
				setFocus(Qt::MouseFocusReason);
				relayout();
			}

			event->accept();

			return;
		}

		++index;
	}

	event->ignore();
}

void VideoGrid::mouseMoveEvent(QMouseEvent *event) {
	m_mouseInside = true;

	const QPoint pos    = event->pos();
	const Layout layout = currentLayout();

	m_lastMousePos = pos;

	// See m_hoveredSlot: only actually redo the show/hide work when the cursor has crossed into a
	// different tile, not on every one of the many move events Qt delivers while it is simply gliding
	// across the one it is already over.
	if (slotAt(pos, layout) != m_hoveredSlot) {
		scheduleHoveredBarUpdate();
	}

	QWidget::mouseMoveEvent(event);
}

void VideoGrid::enterEvent(QEnterEvent *event) {
	m_mouseInside  = true;
	m_lastMousePos = event->position().toPoint();
	scheduleHoveredBarUpdate();

	QWidget::enterEvent(event);
}

void VideoGrid::leaveEvent(QEvent *event) {
	m_mouseInside = false;
	scheduleHoveredBarUpdate();

	QWidget::leaveEvent(event);
}

void VideoGrid::scheduleHoveredBarUpdate() {
	if (m_hoveredBarUpdateScheduled) {
		// Coalesced: Qt delivers a great many move events for one gesture, and every one of them that
		// crosses a tile boundary would otherwise post its own redundant update.
		return;
	}

	m_hoveredBarUpdateScheduled = true;

	// Queued rather than called directly - see the declaration for the crash this exists to prevent. The
	// context object is this widget itself, so a pending update is discarded rather than delivered if the
	// grid is destroyed before the event loop gets back around to it.
	QMetaObject::invokeMethod(
		this,
		[this]() {
			m_hoveredBarUpdateScheduled = false;

			updateHoveredBar();
		},
		Qt::QueuedConnection);
}

void VideoGrid::keyPressEvent(QKeyEvent *event) {
	if (event->key() == Qt::Key_Escape && m_focus != FocusTarget::None) {
		clearFocusedTile();
		event->accept();

		return;
	}

	QWidget::keyPressEvent(event);
}
