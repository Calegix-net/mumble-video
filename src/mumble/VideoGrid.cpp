// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "VideoGrid.h"

#include "Mumble.pb.h"

#include <QtGui/QEnterEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QSlider>
#include <QtWidgets/QToolButton>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

/**
 * The picture-in-picture window a fullscreened tile is actually shown in - a genuine top-level,
 * showFullScreen()'d window covering the whole monitor, not VideoGrid filling its own (dock-sized, not
 * screen-sized) rect the way an early version did. That approach could never fill more than whatever room
 * the video dock happened to have, which on any window smaller than the monitor is not "fullscreen" by
 * any definition a user watching, say, someone else's gameplay would recognise.
 *
 * Deliberately dumb: it owns no state of its own beyond what setContent() was last called with, and reacts
 * to Esc or a double-click by asking its owner to leave fullscreen rather than closing itself - m_focus is
 * the single source of truth for what is fullscreened, in VideoGrid, and this window is just one of the
 * things relayout() brings into line with it.
 */
class FullscreenVideoWindow : public QWidget {
public:
	explicit FullscreenVideoWindow(VideoGrid *owner) : QWidget(owner, Qt::Window), m_owner(owner) {
		setWindowTitle(tr("Mumble"));

		QPalette pal = palette();
		pal.setColor(QPalette::Window, Qt::black);
		setPalette(pal);
		setAutoFillBackground(true);
	}

	void setContent(const QImage &image, const QString &label) {
		m_image = image;
		m_label = label;
		update();
	}

protected:
	void paintEvent(QPaintEvent *) override {
		QPainter painter(this);
		painter.fillRect(rect(), Qt::black);

		if (!m_image.isNull()) {
			const QSize scaled = m_image.size().scaled(size(), Qt::KeepAspectRatio);
			const QRect target((width() - scaled.width()) / 2, (height() - scaled.height()) / 2, scaled.width(),
							   scaled.height());

			painter.drawImage(target, m_image);
		}

		const QRect margin = rect().adjusted(16, 12, -16, -12);

		if (!m_label.isEmpty()) {
			painter.setPen(Qt::white);
			painter.drawText(margin, Qt::AlignTop | Qt::AlignLeft, m_label);
		}

		painter.setPen(QColor(200, 200, 200));
		painter.drawText(margin, Qt::AlignBottom | Qt::AlignRight, tr("Esc or double-click to exit fullscreen"));
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
	QImage m_image;
	QString m_label;
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

QImage VideoGrid::decodeUnit(Surface &surface, const QByteArray &payload) {
	switch (surface.codec) {
		case MumbleProto::VideoState_Codec_TiledImage: {
			QImage tile;

			// Explicitly JPEG rather than letting Qt sniff the format. Sniffing would let a sender pick
			// the decoder by crafting a header, which turns every image format Qt supports into attack
			// surface.
			if (!tile.loadFromData(payload, "JPEG")) {
				return QImage();
			}

			return tile;
		}
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

	if (isNewSurface) {
		bool senderAlreadyPresent = false;

		for (auto it = m_surfaces.cbegin(); it != m_surfaces.cend();) {
			if (it->second.senderSession == senderSession && it->second.sourceKind == sourceKind) {
				// One camera (or screen) per person. A sender restarts a share under a fresh stream id,
				// and if the end of the old one never reached us - the control message was dropped, or
				// the sender's client died - the old surface would sit there as a stuck last frame next
				// to the live one, forever. The newer announcement wins.
				it = m_surfaces.erase(it);
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
	}

	Surface &surface = m_surfaces[key];

	// A codec or source-kind announcement for a stream id already in use replaces it wholesale: a stream
	// id changes whenever the codec, source or dimensions do, so nothing about the old content carries
	// over - least of all a decoder holding reference frames from different content.
	if (surface.streamID != streamID || surface.codec != codec) {
		surface.canvas = QImage();
		surface.vp8.reset();
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
		surface.watching = false;
	}

	surface.senderSession = senderSession;
	surface.streamID      = streamID;
	surface.codec         = codec;
	surface.sourceKind    = sourceKind;
}

void VideoGrid::setWatching(unsigned int senderSession, unsigned int streamID, bool watching) {
	const auto it = m_surfaces.find(surfaceKey(senderSession, streamID));

	if (it == m_surfaces.end() || it->second.watching == watching) {
		return;
	}

	it->second.watching = watching;

	if (!watching && m_focus == FocusTarget::Surface && m_focusedSurfaceKey == it->first) {
		// Un-fullscreen whatever was just told to stop being watched, rather than leaving a placeholder
		// filling the whole grid.
		m_focus = FocusTarget::None;
	}

	emit watchToggled(senderSession, streamID, watching);
	relayout();
}

void VideoGrid::onVideoUnitReceived(unsigned int senderSession, unsigned int streamID, quint64 frameNumber,
									bool isKeyframe, unsigned int x, unsigned int y,
									const QByteArray &encodedTile) {
	// Only streams that announced themselves are decoded. Units arriving for an unannounced stream have
	// no codec, and are dropped rather than assumed to be anything.
	const auto existing = m_surfaces.find(surfaceKey(senderSession, streamID));

	if (existing == m_surfaces.end()) {
		return;
	}

	Surface &surface = existing->second;

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

	const QImage tile = decodeUnit(surface, encodedTile);

	if (tile.isNull()) {
		// Only for codecs this build can decode. An unknown codec fails on every unit forever, and
		// asking its sender for keyframes would be a request nothing can satisfy.
		const bool decodable = surface.codec == MumbleProto::VideoState_Codec_TiledImage
							   || surface.codec == MumbleProto::VideoState_Codec_VP8;

		if (decodable && ++surface.consecutiveFailures >= KEYFRAME_REQUEST_AFTER_FAILURES) {
			// Reset on emit, so a sender that ignores the request is asked again only after another full
			// run of failures rather than on every subsequent unit.
			surface.consecutiveFailures = 0;

			emit keyframeNeeded(senderSession, streamID);
		}

		return;
	}

	surface.consecutiveFailures = 0;

	if (surface.codec == MumbleProto::VideoState_Codec_VP8) {
		surface.lastFrameNumber    = frameNumber;
		surface.hasDecodedFrame    = true;
		surface.awaitingKeyframe   = false;
		surface.unitsWhileAwaiting = 0;
	}

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

	if (wasBlank) {
		emit senderCountChanged(tileCount());
	}

	relayout();
}

void VideoGrid::setSelfCameraFrame(const QImage &frame) {
	const bool wasEmpty = m_selfCameraFrame.isNull();

	m_selfCameraFrame = frame;

	if (wasEmpty != frame.isNull()) {
		emit senderCountChanged(tileCount());
	}

	relayout();
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
	}

	relayout();
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
	if (m_surfaces.erase(surfaceKey(senderSession, streamID)) > 0) {
		emit senderCountChanged(senderCount());
		relayout();
	}
}

void VideoGrid::removeSender(unsigned int senderSession) {
	bool removedAny = false;

	for (auto it = m_surfaces.begin(); it != m_surfaces.end();) {
		if (it->second.senderSession == senderSession) {
			it          = m_surfaces.erase(it);
			removedAny = true;
		} else {
			++it;
		}
	}

	m_senderNames.erase(senderSession);

	if (removedAny) {
		emit senderCountChanged(senderCount());
		relayout();
	}
}

void VideoGrid::clear() {
	m_focus             = FocusTarget::None;
	m_focusedSurfaceKey = 0;

	m_ownCameraControls.reset();
	m_ownScreenControls.reset();
	m_remoteControls.clear();

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
	const int row     = slot / layout.columns;

	return QRect(column * layout.cellWidth, row * layout.cellHeight, layout.cellWidth, layout.cellHeight);
}

int VideoGrid::slotAt(const QPoint &point, const Layout &layout) const {
	if (layout.count == 0 || layout.cellWidth <= 0 || layout.cellHeight <= 0) {
		return -1;
	}

	const int column = point.x() / layout.cellWidth;
	const int row     = point.y() / layout.cellHeight;

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
	constexpr int BAR_HEIGHT = 28;

	// The grid always shows its normal multi-tile layout now, fullscreen or not - see the class comment
	// on FullscreenVideoWindow for why a focused tile no longer makes paintEvent() fill the whole widget
	// with just that one picture. So there is only ever one layout to position bars against here.

	int index = 0;

	if (!m_selfCameraFrame.isNull()) {
		if (!m_ownCameraControls) {
			m_ownCameraControls = makeOwnControlBar(FocusTarget::SelfCamera);
		}

		const QRect cell = cellRect(layout, index++);
		m_ownCameraControls->bar->setGeometry(cell.x(), cell.bottom() - BAR_HEIGHT + 1, cell.width(), BAR_HEIGHT);
		m_ownCameraControls->bar->raise();
	} else {
		m_ownCameraControls.reset();
	}

	if (!m_selfScreenFrame.isNull()) {
		if (!m_ownScreenControls) {
			m_ownScreenControls = makeOwnControlBar(FocusTarget::SelfScreen);
		}

		const QRect cell = cellRect(layout, index++);
		m_ownScreenControls->bar->setGeometry(cell.x(), cell.bottom() - BAR_HEIGHT + 1, cell.width(), BAR_HEIGHT);
		m_ownScreenControls->bar->raise();
	} else {
		m_ownScreenControls.reset();
	}

	// Bars for a sender that no longer holds a surface at all are dropped outright; bars for a surface
	// that is merely unwatched are kept, since the watch button is exactly how a placeholder tile gets
	// out of that state.
	for (auto it = m_remoteControls.begin(); it != m_remoteControls.end();) {
		if (m_surfaces.find(it->first) == m_surfaces.end()) {
			it = m_remoteControls.erase(it);
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
	// Hidden first, unconditionally - simpler than trying to track "which bar was visible last time" and
	// only touch the ones that changed, and this runs at most once per relayout(), not once per frame.
	if (m_ownCameraControls) {
		m_ownCameraControls->bar->setVisible(false);
	}

	if (m_ownScreenControls) {
		m_ownScreenControls->bar->setVisible(false);
	}

	for (auto &entry : m_remoteControls) {
		entry.second->bar->setVisible(false);
	}

	if (!m_mouseInside) {
		m_hoveredSlot = -1;
		return;
	}

	const Layout layout = currentLayout();
	const int slot       = slotAt(m_lastMousePos, layout);

	m_hoveredSlot = slot;

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

	QImage image;
	QString label;

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

	if (image.isNull()) {
		return;
	}

	if (!m_fullscreenWindow) {
		m_fullscreenWindow = new FullscreenVideoWindow(this);
	}

	static_cast< FullscreenVideoWindow * >(m_fullscreenWindow)->setContent(image, label);

	if (!m_fullscreenWindow->isVisible()) {
		m_fullscreenWindow->showFullScreen();
	}
}

QString VideoGrid::labelForSurface(const Surface &surface) const {
	const auto nameIt = m_senderNames.find(surface.senderSession);
	QString label      = nameIt == m_senderNames.end() ? QString() : nameIt->second;

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
	const auto drawPlaceholder = [&](const QRect &cell, const QString &label) {
		painter.fillRect(cell, QColor(32, 32, 32));
		painter.setPen(QColor(180, 180, 180));
		painter.drawText(cell, Qt::AlignCenter | Qt::TextWordWrap,
						 label.isEmpty() ? tr("👁 Click to watch") : tr("%1\n👁 Click to watch").arg(label));
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

void VideoGrid::mouseDoubleClickEvent(QMouseEvent *event) {
	const Layout layout = currentLayout();
	const int slot       = slotAt(event->pos(), layout);

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
		updateHoveredBar();
	}

	QWidget::mouseMoveEvent(event);
}

void VideoGrid::enterEvent(QEnterEvent *event) {
	m_mouseInside  = true;
	m_lastMousePos = event->position().toPoint();
	updateHoveredBar();

	QWidget::enterEvent(event);
}

void VideoGrid::leaveEvent(QEvent *event) {
	m_mouseInside = false;
	updateHoveredBar();

	QWidget::leaveEvent(event);
}

void VideoGrid::keyPressEvent(QKeyEvent *event) {
	if (event->key() == Qt::Key_Escape && m_focus != FocusTarget::None) {
		clearFocusedTile();
		event->accept();

		return;
	}

	QWidget::keyPressEvent(event);
}
