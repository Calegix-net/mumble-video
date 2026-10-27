// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "VideoGrid.h"

#include "AudioOutputScreenShare.h"
#include "Mumble.pb.h"
#include "StreamVolumeSlider.h"
#include "VideoFrameDump.h"

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QDateTime>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFutureWatcher>
#include <QtCore/QMetaObject>
#include <QtCore/QPointer>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtGui/QEnterEvent>
#include <QtGui/QIcon>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPaintEvent>
#include <QtGui/QPainter>
#include <QtGui/QWindow>
#include <QtWidgets/QApplication>
#include <QtWidgets/QGraphicsItem>
#include <QtWidgets/QGraphicsScene>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMenu>
#include <QtWidgets/QSlider>
#include <QtWidgets/QStyle>
#include <QtWidgets/QToolButton>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

static QWidget *makeTileControlStrip(QWidget *parent);

/**
 * The speaker icon for a stream at the given level, from a pair built once per style rather than per call.
 *
 * QStyle::standardIcon() is not a lookup; it builds a QIcon, and on some styles rasterises it. The level
 * label and this icon are refreshed together on every value the slider passes through, which during a drag
 * is every few pixels, on the GUI thread that is also decoding and painting video. Two icons for the two
 * states it can be in cost one build each for the life of the process.
 */
static const QIcon &volumeIcon(QStyle *style, bool muted) {
	static QIcon loud;
	static QIcon silent;
	static QStyle *builtWith = nullptr;

	// Rebuilt if the theme changed under us, which is what swaps the QStyle instance.
	if (builtWith != style) {
		loud      = style->standardIcon(QStyle::SP_MediaVolume);
		silent    = style->standardIcon(QStyle::SP_MediaVolumeMuted);
		builtWith = style;
	}

	return muted ? silent : loud;
}

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

	/// The volume control the bar shows: whether there is one (a stream with audio of its own - a screen
	/// share, not a camera), and its level in percent.
	struct VolumeState {
		bool available = false;
		int percent    = 0;
	};
	using VolumeProvider = std::function< VolumeState() >;
	using VolumeSetter   = std::function< void(int percent) >;

	/// About 60 repaints a second at most, so a share sent at 60 fps is shown at 60 - at 33 ms this window
	/// was the cap, at 30 fps (21 on Windows, before the timer was made precise). A paint here is a real,
	/// CPU-side scale of the sender's whole desktop onto the whole monitor when everything is dirty, so it
	/// happens once per frame received and no more often than this; a 30 fps share still costs 30.
	static constexpr int REPAINT_INTERVAL_MSEC = 16;
	/// The control bar hides after the mouse has been still this long, like a video player's.
	static constexpr int CONTROLS_HIDE_MSEC = 3000;
	static constexpr int CONTROLS_HEIGHT    = 44;
	/// How often the overlay's contents are brought up to date while it is showing. Ten times a second:
	/// fast enough that a talking indicator does not read as broken, and a fraction of what the real
	/// overlay does, which refreshes the same item once per game frame.
	static constexpr int OVERLAY_REFRESH_MSEC = 100;

	/// What asking this window to go away means. The focus-driven fullscreen window gives up the grid's
	/// focus; a picture-in-picture window puts its own tile back in the grid.
	using CloseHandler = std::function< void() >;

	/// @param detached True for a picture-in-picture window: a real top-level window with its own taskbar
	///   entry, which can therefore be minimised and restored on its own, and whose tile has left the
	///   grid. Parentless for exactly that reason - a window owned by the grid is a transient of the main
	///   window, which on every platform means no taskbar button of its own and minimising along with its
	///   parent, which is the opposite of what a picture-in-picture window is for.
	explicit FullscreenVideoWindow(VideoGrid *owner, ContentProvider provider, VolumeProvider volume,
								   VolumeSetter setVolume, bool detached = false, CloseHandler onClose = {})
		: QWidget(detached ? nullptr : owner, Qt::Window), m_owner(owner), m_provider(std::move(provider)),
		  m_volume(std::move(volume)), m_setVolume(std::move(setVolume)), m_detached(detached),
		  m_onClose(std::move(onClose)) {
		setWindowTitle(tr("Mumble"));

		QPalette pal = palette();
		pal.setColor(QPalette::Window, Qt::black);
		setPalette(pal);
		setAutoFillBackground(true);
		// Moving the mouse, without a button held, is what brings the controls back.
		setMouseTracking(true);

		m_repaintTimer = new QTimer(this);
		m_repaintTimer->setSingleShot(true);
		m_repaintTimer->setTimerType(Qt::PreciseTimer);
		m_repaintTimer->setInterval(REPAINT_INTERVAL_MSEC);
		connect(m_repaintTimer, &QTimer::timeout, this, &FullscreenVideoWindow::flushRepaint);

		buildControls();
	}

	/// Names the window after whoever it is showing, so its taskbar entry can be told apart from the
	/// other picture-in-picture windows and from the main one.
	void refreshWindowTitle() {
		QImage image;
		QString label;
		m_provider(image, label);

		setWindowTitle(label.isEmpty() ? tr("Mumble") : tr("%1 - Mumble").arg(label));
	}

	/// The whole picture is different - a different tile was focused, or the stream restarted. Everything
	/// is repainted, on the next coalesced repaint.
	void contentReplaced() {
		m_wholeDirty = true;
		scheduleRepaint();
		refreshVolume();
		showControls();
	}

	/// Shows this window either filling the monitor or as an ordinary movable, resizable window, and
	/// switches between the two in place. The pop-out is exactly this class in its windowed presentation:
	/// everything else about it - the pull-based content provider, the coalesced repaints, the volume
	/// control, the auto-hiding bar - is wanted identically either way, so none of it is duplicated.
	void setPresentation(bool windowed) {
		const bool changed = windowed != m_windowed || !isVisible();

		m_windowed = windowed;

		if (!changed) {
			return;
		}

		if (windowed) {
			if (!m_hasWindowedSize) {
				// Big enough to be worth watching, small enough to leave the rest of the desktop usable,
				// which is the entire point of asking for a window instead of fullscreen.
				resize(960, 540);
				m_hasWindowedSize = true;
			}

			showNormal();
			raise();
		} else {
			showFullScreen();
		}

		applyPresentationLabels();
		showControls();
	}

	/// Turns the overlay on or off over this window, building it on demand through the owner's factory so
	/// that a viewer who never asks for it pays nothing, and tearing it down when switched off so it stops
	/// costing a refresh.
	void setOverlayEnabled(bool enabled) {
		if (enabled == (m_overlayScene != nullptr)) {
			applyPresentationLabels();

			return;
		}

		if (!enabled) {
			delete m_overlayScene;
			m_overlayScene = nullptr;
			m_overlayItem  = nullptr;

			if (m_overlayTimer) {
				m_overlayTimer->stop();
			}

			m_wholeDirty = true;
			scheduleRepaint();
			applyPresentationLabels();

			return;
		}

		QGraphicsItem *const item = m_owner->buildOverlayItem();

		if (!item) {
			// Built without overlay support, or the owner declined. Nothing to show, nothing to undo.
			applyPresentationLabels();

			return;
		}

		m_overlayScene = new QGraphicsScene(this);
		m_overlayScene->setBackgroundBrush(Qt::NoBrush);
		m_overlayScene->addItem(item);
		item->show();
		m_overlayItem = item;

		if (!m_overlayTimer) {
			m_overlayTimer = new QTimer(this);
			m_overlayTimer->setInterval(OVERLAY_REFRESH_MSEC);
			connect(m_overlayTimer, &QTimer::timeout, this, &FullscreenVideoWindow::refreshOverlay);
		}

		resizeOverlayScene();
		m_overlayTimer->start();
		refreshOverlay();
		applyPresentationLabels();
	}

	/// The overlay sizes itself from the scene rect, exactly as it sizes itself from a game's resolution
	/// when drawn over one, so the scene has to track this window.
	void resizeOverlayScene() {
		if (!m_overlayScene) {
			return;
		}

		m_overlayScene->setSceneRect(QRectF(0.0, 0.0, width(), height()));
		m_owner->refreshOverlayItem(m_overlayItem, QSizeF(width(), height()));
	}

	/// Brings the overlay up to date - who is talking, who joined, who left - and repaints.
	void refreshOverlay() {
		if (!m_overlayItem) {
			return;
		}

		m_owner->refreshOverlayItem(m_overlayItem, QSizeF(width(), height()));

		// The overlay can sit anywhere in the window, and its contents move independently of which video
		// tiles changed, so there is no useful dirty rectangle for it.
		m_wholeDirty = true;
		scheduleRepaint();
	}

	/// The level changed somewhere else - the tile's own slider - so the bar's follows it.
	void refreshVolume() {
		const VolumeState state = m_volume ? m_volume() : VolumeState();

		m_muteButton->setVisible(state.available);
		m_volumeSlider->setVisible(state.available);
		m_volumeLabel->setVisible(state.available);

		if (state.available) {
			const QSignalBlocker blocker(m_volumeSlider);
			m_volumeSlider->setValue(state.percent);
			showLevel(state.percent);
			if (state.percent > 0) {
				m_volumeBeforeMute = state.percent;
			}
		}
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

		// The overlay, composited straight into this paint out of a scene that is deliberately never put
		// in a QGraphicsView. The overlay's user group carries its own wheel and context-menu handlers,
		// which edit the overlay's configuration - so a view would mean scrolling over somebody else's
		// screen share silently resized the viewer's overlay. With no view, no event reaches the item at
		// all and it is strictly something to look at.
		if (m_overlayScene) {
			m_overlayScene->render(&painter, rect(), m_overlayScene->sceneRect());
		}
	}

	void resizeEvent(QResizeEvent *event) override {
		QWidget::resizeEvent(event);
		m_wholeDirty = true;
		m_controls->setGeometry(0, height() - CONTROLS_HEIGHT, width(), CONTROLS_HEIGHT);
		resizeOverlayScene();
	}

	void showEvent(QShowEvent *event) override {
		QWidget::showEvent(event);
		refreshVolume();
		showControls();
		refreshWindowTitle();

		// Read on every show rather than once at construction: the preference is persisted, and this
		// window outlives any one viewing of a tile.
		setOverlayEnabled(m_owner->videoOverlayEnabled());
	}

	/// In the windowed presentation the picture itself is a handle to move the window by, like a video
	/// player's. Needed on Wayland in particular: there a client cannot place its own window, and whether
	/// it gets a title bar to drag at all depends on the compositor and on Qt's decoration plugin, so a
	/// pop-out could end up with nothing to move it by. Armed on the press and started only once the mouse
	/// has travelled the drag distance, so a double-click (pop back in) still arrives as one.
	void mousePressEvent(QMouseEvent *event) override {
		if (m_windowed && event->button() == Qt::LeftButton) {
			m_dragArmed       = true;
			m_dragSystemMove  = false;
			m_dragPressGlobal = event->globalPosition().toPoint();
			m_dragWindowStart = pos();
			event->accept();

			return;
		}

		QWidget::mousePressEvent(event);
	}

	void mouseMoveEvent(QMouseEvent *event) override {
		showControls();

		if (m_dragArmed && (event->buttons() & Qt::LeftButton)) {
			const QPoint travelled = event->globalPosition().toPoint() - m_dragPressGlobal;

			if (!m_dragSystemMove && !m_dragManual
				&& travelled.manhattanLength() >= QApplication::startDragDistance()) {
				// The compositor's own move, which is the only kind Wayland allows and is also what X11 and
				// Windows do best (snapping, edge resistance). Where the platform has none - the offscreen
				// platform the tests run on, an old X11 window manager - the window is moved by hand.
				QWindow *handle  = windowHandle();
				m_dragSystemMove = handle && handle->startSystemMove();
				m_dragManual     = !m_dragSystemMove;
			}

			if (m_dragSystemMove) {
				// The compositor has the pointer until the button is released.
				m_dragArmed = false;
			} else if (m_dragManual) {
				move(m_dragWindowStart + travelled);
			}

			event->accept();

			return;
		}

		QWidget::mouseMoveEvent(event);
	}

	void mouseReleaseEvent(QMouseEvent *event) override {
		if (event->button() == Qt::LeftButton) {
			m_dragArmed      = false;
			m_dragManual     = false;
			m_dragSystemMove = false;
		}

		QWidget::mouseReleaseEvent(event);
	}

	/// Asked to go away: Esc, a double-click, the close button, or the title bar's own close.
	void requestClose() {
		if (m_onClose) {
			m_onClose();

			return;
		}

		m_owner->clearFocusedTile();
	}

	void keyPressEvent(QKeyEvent *event) override {
		if (event->key() == Qt::Key_Escape) {
			requestClose();

			return;
		}

		QWidget::keyPressEvent(event);
	}

	void mouseDoubleClickEvent(QMouseEvent *) override { requestClose(); }

	/// Only reachable in the windowed presentation - fullscreen has no title bar to close from. Without
	/// this the window would hide while m_focus still pointed at the tile, so the grid would go on
	/// treating it as focused and the next relayout() would put the window straight back up.
	void closeEvent(QCloseEvent *event) override {
		event->ignore();
		requestClose();
	}

private:
	/// The bar along the bottom: the stream's volume (speaker button, slider, level), and a button out of
	/// fullscreen - which double-click and Esc also are, but a button is what people look for.
	void buildControls() {
		m_controls = makeTileControlStrip(this);
		m_controls->setMouseTracking(true);

		auto *layout = new QHBoxLayout(m_controls);
		layout->setContentsMargins(12, 4, 12, 4);
		layout->setSpacing(8);

		m_muteButton = new QToolButton(m_controls);
		m_muteButton->setAutoRaise(true);
		m_muteButton->setIconSize(QSize(20, 20));
		m_muteButton->setFocusPolicy(Qt::NoFocus);
		m_muteButton->setAccessibleName(tr("Mute this stream"));
		layout->addWidget(m_muteButton);

		m_volumeSlider = new StreamVolumeSlider(m_controls);
		m_volumeSlider->setRange(0, static_cast< int >(AudioOutputScreenShare::MAX_VOLUME * 100.0f));
		m_volumeSlider->setSingleStep(5);
		m_volumeSlider->setPageStep(25);
		m_volumeSlider->setFixedWidth(160);
		m_volumeSlider->setFocusPolicy(Qt::NoFocus);
		m_volumeSlider->setToolTip(tr("Stream volume"));
		m_volumeSlider->setAccessibleName(tr("Stream volume"));
		layout->addWidget(m_volumeSlider);

		m_volumeLabel = new QLabel(m_controls);
		m_volumeLabel->setFixedWidth(m_volumeLabel->fontMetrics().horizontalAdvance(QStringLiteral("200%")) + 4);
		layout->addWidget(m_volumeLabel);

		layout->addStretch();

		// Words, not just an icon: the theme's "restore" icon is small and grey and was easy to miss on top of
		// video. The glyph is the one the tile's own fullscreen button uses.
		const QString buttonStyle =
			QStringLiteral("QToolButton { color: white; padding: 4px 10px; border-radius: 4px; }"
						   "QToolButton:hover { background: rgba(255, 255, 255, 40); }");

		// Switches between filling the monitor and being an ordinary window, without leaving the view.
		// Popping a tile out and then wanting it fullscreen (or the reverse) should not mean going back to
		// the grid to press a different button.
		// Only offered when the client was actually built with the overlay.
		if (m_owner->videoOverlayAvailable()) {
			m_overlayButton = new QToolButton(m_controls);
			m_overlayButton->setAutoRaise(true);
			m_overlayButton->setCursor(Qt::PointingHandCursor);
			m_overlayButton->setStyleSheet(buttonStyle);
			m_overlayButton->setFocusPolicy(Qt::NoFocus);
			layout->addWidget(m_overlayButton);

			// Asked of the owner rather than decided here: it holds the persisted preference, and calls
			// setVideoOverlayEnabled() back once it has recorded the answer.
			connect(m_overlayButton, &QToolButton::clicked, m_owner,
					[owner = m_owner]() { emit owner->videoOverlayToggleRequested(); });
		}

		// Only a picture-in-picture window has two presentations to switch between; the focus-driven
		// window is always fullscreen. Wanting a popped-out stream to fill the screen for a moment, and
		// then go back to a corner, should not mean putting it back in the grid first.
		if (m_detached) {
			m_modeButton = new QToolButton(m_controls);
			m_modeButton->setAutoRaise(true);
			m_modeButton->setCursor(Qt::PointingHandCursor);
			m_modeButton->setStyleSheet(buttonStyle);
			m_modeButton->setFocusPolicy(Qt::NoFocus);
			layout->addWidget(m_modeButton);

			connect(m_modeButton, &QToolButton::clicked, this, [this]() { setPresentation(!m_windowed); });
		}

		// Words, not just an icon: the theme's "restore" icon is small and grey and was easy to miss on top of
		// video. The glyph is the one the tile's own fullscreen button uses.
		m_exitButton = new QToolButton(m_controls);
		m_exitButton->setAutoRaise(true);
		m_exitButton->setCursor(Qt::PointingHandCursor);
		m_exitButton->setStyleSheet(buttonStyle);
		m_exitButton->setFocusPolicy(Qt::NoFocus);
		layout->addWidget(m_exitButton);

		connect(m_exitButton, &QToolButton::clicked, this, [this]() { requestClose(); });

		applyPresentationLabels();

		connect(m_volumeSlider, &QSlider::valueChanged, this, [this](int value) {
			if (value > 0) {
				m_volumeBeforeMute = value;
			}
			showLevel(value);
			if (m_setVolume) {
				m_setVolume(value);
			}
		});

		connect(m_muteButton, &QToolButton::clicked, this,
				[this]() { m_volumeSlider->setValue(m_volumeSlider->value() == 0 ? m_volumeBeforeMute : 0); });

		m_hideTimer = new QTimer(this);
		m_hideTimer->setSingleShot(true);
		m_hideTimer->setInterval(CONTROLS_HIDE_MSEC);
		connect(m_hideTimer, &QTimer::timeout, this, [this]() {
			// Not while the pointer is on the bar itself: someone is about to use it.
			if (!m_controls->underMouse()) {
				m_controls->hide();
				setCursor(Qt::BlankCursor);
			} else {
				m_hideTimer->start();
			}
		});

		m_controls->hide();
	}

	void showControls() {
		m_controls->setGeometry(0, height() - CONTROLS_HEIGHT, width(), CONTROLS_HEIGHT);
		m_controls->show();
		m_controls->raise();
		unsetCursor();
		m_hideTimer->start();
	}

	/// Called for every value the slider passes through, so it does as little as it can get away with:
	/// the icon and its tooltip only say muted or not, and only change when that does.
	void showLevel(int value) {
		const QString level = tr("%1%").arg(value);
		m_volumeLabel->setText(level);
		m_volumeSlider->setLevelText(level);

		const bool muted = value == 0;

		if (m_shownAsMuted != muted) {
			m_shownAsMuted = muted;
			m_muteButton->setIcon(volumeIcon(style(), muted));
			m_muteButton->setToolTip(muted ? tr("Unmute stream") : tr("Mute stream"));
		}
	}

	VideoGrid *m_owner;
	ContentProvider m_provider;
	VolumeProvider m_volume;
	VolumeSetter m_setVolume;
	QTimer *m_repaintTimer = nullptr;

	/// Keeps the two buttons describing what they actually do in the current presentation, rather than
	/// setting their text once, because the same window serves both.
	void applyPresentationLabels() {
		if (!m_exitButton) {
			return;
		}

		if (m_modeButton) {
			m_modeButton->setText(m_windowed ? tr("⛶  Fullscreen") : tr("❐  Windowed"));
			m_modeButton->setToolTip(m_windowed ? tr("Fill the screen") : tr("Back to a window"));
		}

		// A picture-in-picture window closes; the focus-driven one gives the tile back to the grid.
		m_exitButton->setText(m_detached ? tr("✕  Close") : tr("⛶  Exit fullscreen"));
		m_exitButton->setToolTip(m_detached ? tr("Close this window (Esc or double-click)")
										   : tr("Exit fullscreen (Esc or double-click)"));

		if (m_overlayButton) {
			const bool on = m_overlayScene != nullptr;

			m_overlayButton->setText(on ? tr("◉  Overlay") : tr("○  Overlay"));
			m_overlayButton->setToolTip(on ? tr("Hide the Mumble overlay")
										   : tr("Show the Mumble overlay, as it appears over a game"));
		}
	}

	QWidget *m_controls       = nullptr;
	QToolButton *m_overlayButton = nullptr;
	QToolButton *m_modeButton    = nullptr;
	QToolButton *m_exitButton = nullptr;
	QToolButton *m_muteButton = nullptr;
	StreamVolumeSlider *m_volumeSlider = nullptr;
	QLabel *m_volumeLabel     = nullptr;
	QTimer *m_hideTimer       = nullptr;
	int m_volumeBeforeMute    = static_cast< int >(AudioOutputScreenShare::DEFAULT_VOLUME * 100.0f);

	/// What the speaker button is currently drawn as - see showLevel(), which only touches the icon when
	/// this disagrees with the level. Starts as neither, so the first level sets it whichever way it goes.
	std::optional< bool > m_shownAsMuted;

	/// Which presentation is showing. Fullscreen is the default, so the existing fullscreen button
	/// behaves exactly as it did before the pop-out existed.
	bool m_windowed = false;

	/// Whether a windowed size has been picked yet. Set once, so a user who resizes or moves the pop-out
	/// keeps that geometry across a pop-in and back out.
	bool m_hasWindowedSize = false;

	/// Whether this is a picture-in-picture window rather than the focus-driven fullscreen one.
	bool m_detached = false;

	/// A drag of the picture moving the windowed presentation - see mousePressEvent().
	bool m_dragArmed      = false;
	bool m_dragSystemMove = false;
	bool m_dragManual     = false;
	QPoint m_dragPressGlobal;
	QPoint m_dragWindowStart;

	/// What closing means. See CloseHandler.
	CloseHandler m_onClose;

	/// The overlay, or null while it is switched off. Never attached to a QGraphicsView - see paintEvent().
	/// The scene owns the item; m_overlayItem is a borrowed pointer, for handing back to the refresher.
	QGraphicsScene *m_overlayScene = nullptr;
	QGraphicsItem *m_overlayItem   = nullptr;
	QTimer *m_overlayTimer         = nullptr;

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
	// paintEvent() covers every pixel (it starts by filling the whole widget), so nothing behind the grid
	// ever needs painting first. Without this, each incoming tile's update() also repainted the dock widget
	// holding the grid - title bar included, which with the Fusion style re-reads the title bar's button
	// icons from disk every time: measured as most of a viewer's GUI-thread time while watching a share.
	setAttribute(Qt::WA_OpaquePaintEvent);

	// Small floor, greedy ceiling: the panel must never be what stops the main window from being made
	// smaller, and given room it should take it. Tiles scale to whatever they get (see paintEvent).
	setMinimumSize(160, 90);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

	m_gridRepaintTimer = new QTimer(this);
	m_gridRepaintTimer->setSingleShot(true);
	m_gridRepaintTimer->setTimerType(Qt::PreciseTimer);
	m_gridRepaintTimer->setInterval(GRID_REPAINT_INTERVAL_MSEC);
	connect(m_gridRepaintTimer, &QTimer::timeout, this, &VideoGrid::flushGridRepaint);

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

	// Measurement only, gated like the mock sources so production never pays for it.
	if (!qEnvironmentVariableIsEmpty("MUMBLE_VIDEO_STATS")) {
		m_statsTimer = new QTimer(this);
		connect(m_statsTimer, &QTimer::timeout, this, &VideoGrid::logStats);
		m_statsTimer->start(2000);

		// How late a 10 ms timer fires on the GUI thread: every millisecond late is a millisecond the
		// window could not paint or react to input - what stutter is, measured. "GuiLag" every 2 seconds.
		auto *lagTimer = new QTimer(this);
		lagTimer->setTimerType(Qt::PreciseTimer);
		auto lastTick  = std::make_shared< QElapsedTimer >();
		auto worstMsec = std::make_shared< qint64 >(0);
		auto lateTicks = std::make_shared< int >(0);
		auto window    = std::make_shared< QElapsedTimer >();
		lastTick->start();
		window->start();
		connect(lagTimer, &QTimer::timeout, this, [lastTick, worstMsec, lateTicks, window]() {
			const qint64 late = lastTick->restart() - 10;
			*worstMsec        = std::max(*worstMsec, late);
			*lateTicks += late >= 16 ? 1 : 0;
			if (window->elapsed() >= 2000) {
				qInfo("GuiLag worstMs=%lld ticksOver16ms=%d", static_cast< long long >(*worstMsec), *lateTicks);
				*worstMsec = 0;
				*lateTicks = 0;
				window->restart();
			}
		});
		lagTimer->start(10);
	}
}

void VideoGrid::noteStatsPaint(Surface &surface, quint64 frameNumber) {
	if (!m_statsTimer) {
		return;
	}

	Surface::Stats &stats = surface.stats;
	const qint64 now      = QDateTime::currentMSecsSinceEpoch();

	++stats.tilesPainted;
	if (frameNumber > stats.newestPaintedFrame) {
		++stats.framesPainted;
		stats.newestPaintedFrame = frameNumber;
	}
	if (stats.lastPaintMsec > 0) {
		stats.maxPaintGapMsec = std::max(stats.maxPaintGapMsec, now - stats.lastPaintMsec);
	}
	stats.lastPaintMsec = now;

	// One sample per frame, when its first tile is painted: that is the moment the frame became visible,
	// and a tiled codec calls this once per tile. Erasing up to and including the frame found is what makes
	// it the first - and it also discards the capture times of any older frames still waiting, which by now
	// are never going to be painted.
	const auto capture = stats.captureUsecByFrame.find(frameNumber);

	if (capture != stats.captureUsecByFrame.end()) {
		const qint64 floor = std::min(stats.arrivalFloorMsec, stats.previousArrivalFloorMsec);

		// No floor yet would mean no unit has been timed, which cannot be the case here - this frame's own
		// capture time is in the map - but a sample measured against the sentinel would be nonsense, so it
		// is checked rather than assumed.
		if (floor != std::numeric_limits< qint64 >::max() && stats.paintDelayMsec.size() < MAX_LATENCY_SAMPLES) {
			stats.paintDelayMsec.push_back(now - static_cast< qint64 >(capture->second / 1000) - floor);
		}

		stats.captureUsecByFrame.erase(stats.captureUsecByFrame.begin(), std::next(capture));
	}
}

/// The sample at the given fraction of the way through @p sorted, or -1 when there are none.
///
/// Nearest-rank, not interpolated: these are millisecond latencies, and an interpolated figure that no
/// frame actually took would be harder to reason about than one that some frame did.
static qint64 percentileMsec(const std::vector< qint64 > &sorted, double fraction) {
	if (sorted.empty()) {
		return -1;
	}

	const std::size_t index =
		std::min(sorted.size() - 1, static_cast< std::size_t >(fraction * static_cast< double >(sorted.size())));

	return sorted[index];
}

void VideoGrid::dumpPaintedFrame(const Surface &surface, quint64 frameNumber, const QImage &image) {
	if (m_frameDumpDirectory.isEmpty()) {
		return;
	}

	// Whole-frame codecs only. A tiled stream's "frame" arrives as up to 135 separate pictures with no
	// moment at which one of them is the frame, so saving any of them under the frame's number would
	// produce a file that is not what the sender encoded and would be compared against it anyway.
	if (surface.codec != MumbleProto::VideoState_Codec_VP8 && surface.codec != MumbleProto::VideoState_Codec_VP9
		&& surface.codec != MumbleProto::VideoState_Codec_H264) {
		return;
	}

	VideoFrameDump::write(m_frameDumpDirectory, "rx", surface.streamID, frameNumber, image);
}

void VideoGrid::logStats() {
	const qint64 now = QDateTime::currentMSecsSinceEpoch();

	for (auto &entry : m_surfaces) {
		Surface &surface = entry.second;
		if (!surface.watching) {
			continue;
		}

		Surface::Stats &stats = surface.stats;
		// A gap still open right now counts too: a stream that froze and stayed frozen never paints again
		// to close it.
		const qint64 openGap = stats.lastPaintMsec > 0 ? now - stats.lastPaintMsec : 0;

		// Sorted in place: these samples are reset below and nothing else reads them.
		std::sort(stats.paintDelayMsec.begin(), stats.paintDelayMsec.end());

		// The playout code's own estimate of the same floor, from the same arithmetic over a different
		// window. Printed beside this one because the two disagreeing is itself worth seeing. -1 when
		// nothing has been timed yet, rather than the sentinel maximum.
		const qint64 offset       = std::min(surface.playout.windowMin, surface.playout.previousMin);
		const qint64 rawFloor     = std::min(stats.arrivalFloorMsec, stats.previousArrivalFloorMsec);
		const qint64 arrivalFloor = rawFloor == std::numeric_limits< qint64 >::max() ? -1 : rawFloor;

		qInfo("VideoStats rx %u/%u codec=%d units=%llu tiles=%llu frames=%llu newestUnit=%llu newestPainted=%llu "
			  "maxGapMs=%lld kfOverload=%llu kfLoss=%llu kfFail=%llu pending=%zu awaitingKey=%d gapsRecovered=%u "
			  "gapsAbandoned=%u held=%zu paintDelayMsP50=%lld paintDelayMsP90=%lld paintDelayMsMax=%lld "
			  "paintDelaySamples=%zu arrivalFloorMs=%lld playoutOffsetMs=%lld",
			  surface.senderSession, surface.streamID, surface.codec, static_cast< unsigned long long >(stats.units),
			  static_cast< unsigned long long >(stats.tilesPainted),
			  static_cast< unsigned long long >(stats.framesPainted),
			  static_cast< unsigned long long >(stats.newestUnitFrame),
			  static_cast< unsigned long long >(stats.newestPaintedFrame),
			  static_cast< long long >(std::max(stats.maxPaintGapMsec, openGap)),
			  static_cast< unsigned long long >(stats.keyframeRequestsOverload),
			  static_cast< unsigned long long >(stats.keyframeRequestsLoss),
			  static_cast< unsigned long long >(stats.keyframeRequestsFailure), surface.pendingTiles.size(),
			  surface.awaitingKeyframe ? 1 : 0, surface.vp8GapsRecovered, surface.vp8GapsAbandoned,
			  surface.vp8Held.size(), static_cast< long long >(percentileMsec(stats.paintDelayMsec, 0.5)),
			  static_cast< long long >(percentileMsec(stats.paintDelayMsec, 0.9)),
			  static_cast< long long >(stats.paintDelayMsec.empty() ? -1 : stats.paintDelayMsec.back()),
			  stats.paintDelayMsec.size(), static_cast< long long >(arrivalFloor),
			  static_cast< long long >(offset == std::numeric_limits< qint64 >::max() ? -1 : offset));

		stats.paintDelayMsec.clear();
		stats.units = stats.tilesPainted = stats.framesPainted = 0;
		stats.keyframeRequestsOverload = stats.keyframeRequestsLoss = stats.keyframeRequestsFailure = 0;
		stats.maxPaintGapMsec                                                                       = 0;
	}
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
		if (occupiesCell(it->second)) {
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

/// Whether one unit of this codec carries a whole picture rather than a tile of one.
///
/// It decides whether a decoded unit replaces the canvas or is painted into it, which is what makes a
/// resolution change mid-stream safe for some codecs and not others: a whole frame simply becomes the new
/// picture, whatever its shape, while a tile is positioned within a picture whose extent the receiver is
/// inferring - so a tiled sender must still take a new stream id when its dimensions change.
///
/// Not in the header with occupiesCell(): Surface::codec is a plain int there precisely so that the
/// header need not pull in the generated protocol definitions.
static bool isWholeFrameCodec(int codec) {
	return codec == MumbleProto::VideoState_Codec_VP8 || codec == MumbleProto::VideoState_Codec_VP9
		   || codec == MumbleProto::VideoState_Codec_H264;
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

bool VideoGrid::prepareDecoder(Surface &surface) {
	switch (surface.codec) {
		case MumbleProto::VideoState_Codec_VP8:
		case MumbleProto::VideoState_Codec_VP9: {
			const VpxCodec codec = surface.codec == MumbleProto::VideoState_Codec_VP9 ? VpxCodec::VP9 : VpxCodec::VP8;

			if (!surface.vp8 || surface.vp8->codec() != codec) {
				surface.vp8 = std::make_shared< VpxDecoder >(codec);
			}

			return surface.vp8->isValid();
		}
#ifdef USE_H264
		case MumbleProto::VideoState_Codec_H264:
			if (!surface.h264) {
				surface.h264 = std::make_shared< H264Decoder >();
			}

			return surface.h264->isValid();
#endif
		default:
			// CODEC_UNKNOWN, or a codec this build has no decoder for. Dropped rather than guessed at,
			// as the protocol requires.
			return false;
	}
}

QImage VideoGrid::decodeUnit(Surface &surface, const QByteArray &payload) {
	if (surface.codec == MumbleProto::VideoState_Codec_TiledImage) {
		return decodeJpegTile(payload);
	}

	if (!prepareDecoder(surface)) {
		return QImage();
	}

#ifdef USE_H264
	if (surface.codec == MumbleProto::VideoState_Codec_H264) {
		return surface.h264->decode(payload);
	}
#endif

	const auto *bytes = reinterpret_cast< const Mumble::Protocol::byte * >(payload.constData());

	return surface.vp8->decode(
		std::vector< Mumble::Protocol::byte >(bytes, bytes + static_cast< std::size_t >(payload.size())));
}

VideoGrid::~VideoGrid() {
	// Picture-in-picture windows are parentless top-level windows, so Qt's parent-child ownership never
	// reaches them and nothing else is going to close them. Their content callbacks read this grid, so
	// one left open outlives what it reads from - and it keeps its own repaint timer, which calls that
	// callback a few times a second. The result is a window painting from freed memory for as long as
	// anything still pumps an event loop, which on the way out of the client is the rest of shutdown.
	//
	// Deleted outright rather than through destroyPipWindow(), whose deleteLater() is right for every
	// other caller - they are reached from a window's own close handler - but here would leave exactly
	// the window this is closing alive until an event loop that may never run again.
	while (!m_pipWindows.empty()) {
		const auto it   = m_pipWindows.begin();
		QWidget *window = it->second.window;

		// Erased before the delete, so nothing reached from the window's teardown can find it here.
		m_pipWindows.erase(it);

		window->hide();
		delete window;
	}

	// The decode thread may be working on a frame; it finishes that one, and its result, queued to this
	// object, is discarded with it.
	if (m_decodeThread) {
		m_decodeThread->quit();
		m_decodeThread->wait();
	}
}

void VideoGrid::setThreadedDecoding(bool threaded) {
	if (threaded == (m_decodeThread != nullptr)) {
		return;
	}

	if (!threaded) {
		m_decodeThread->quit();
		m_decodeThread->wait();
		delete m_decodeThread;
		m_decodeThread = nullptr;
		m_decodeWorker = nullptr;
		return;
	}

	m_playoutTimer = new QTimer(this);
	m_playoutTimer->setSingleShot(true);
	m_playoutTimer->setTimerType(Qt::PreciseTimer);
	connect(m_playoutTimer, &QTimer::timeout, this, &VideoGrid::flushPlayout);

	m_decodeThread = new QThread(this);
	m_decodeThread->setObjectName(QStringLiteral("VideoDecode"));
	// An object living on the thread, for queued calls to run there. Deleted with the thread's event loop.
	m_decodeWorker = new QObject();
	m_decodeWorker->moveToThread(m_decodeThread);
	connect(m_decodeThread, &QThread::finished, m_decodeWorker, &QObject::deleteLater);
	m_decodeThread->start();
}

void VideoGrid::noteDecodeFailure(Surface &surface) {
	if (++surface.consecutiveFailures >= KEYFRAME_REQUEST_AFTER_FAILURES) {
		// Reset on emit, so a sender that ignores the request is asked again only after another full run
		// of failures rather than on every subsequent unit.
		surface.consecutiveFailures = 0;

		++surface.stats.keyframeRequestsFailure;
		emit keyframeNeeded(surface.senderSession, surface.streamID);
	}
}

void VideoGrid::onFrameDecoded(std::uint64_t key, std::uint64_t epoch, quint64 frameNumber, unsigned int x,
							   unsigned int y, bool decoded, const QImage &image) {
	const auto it = m_surfaces.find(key);

	if (it == m_surfaces.end() || it->second.decodeEpoch != epoch || !it->second.watching) {
		// The stream ended, started over or stopped being watched while this frame was being decoded.
		return;
	}

	Surface &surface = it->second;

	if (!decoded) {
		// Undecodable - typically a frame whose reference was itself damaged. The frames after it fail the
		// same way until a keyframe, which a run of failures asks for.
		noteDecodeFailure(surface);
		return;
	}

	surface.consecutiveFailures = 0;

	if (image.isNull()) {
		// Decoded but not converted: the decode thread had fallen well behind, and a newer frame is shown.
		return;
	}

	presentFrame(key, surface, frameNumber, x, y, image);
}

void VideoGrid::resetPlayout(Surface &surface) {
	surface.playout = Surface::Playout();
}

void VideoGrid::presentFrame(std::uint64_t key, Surface &surface, quint64 frameNumber, unsigned int x, unsigned int y,
							 const QImage &image) {
	Surface::Playout &playout = surface.playout;
	const auto capture        = playout.captureUsec.find(frameNumber);
	const qint64 offset       = std::min(playout.windowMin, playout.previousMin);

	if (capture == playout.captureUsec.end() || offset == std::numeric_limits< qint64 >::max()) {
		noteStatsPaint(surface, frameNumber);
		dumpPaintedFrame(surface, frameNumber, image);
		applyDecodedTile(key, surface, x, y, image);
		return;
	}

	const qint64 due = static_cast< qint64 >(capture->second / 1000) + offset + PLAYOUT_DELAY_MSEC;
	playout.captureUsec.erase(playout.captureUsec.begin(), std::next(capture));

	Surface::Playout::Pending pending;
	pending.image              = image;
	pending.x                  = x;
	pending.y                  = y;
	pending.dueMsec            = due;
	playout.queue[frameNumber] = pending;

	flushPlayout();
}

void VideoGrid::flushPlayout() {
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	qint64 nextDue   = std::numeric_limits< qint64 >::max();

	for (auto &entry : m_surfaces) {
		Surface &surface = entry.second;
		auto &queue      = surface.playout.queue;

		while (!queue.empty()) {
			const auto front = queue.begin();

			// Its time has come - or the queue is longer than any re-send can explain, and the oldest goes now
			// rather than the picture falling further behind.
			if (front->second.dueMsec > now && queue.size() <= MAX_PLAYOUT_QUEUE) {
				nextDue = std::min(nextDue, front->second.dueMsec);
				break;
			}

			const quint64 frameNumber               = front->first;
			const Surface::Playout::Pending pending = front->second;
			queue.erase(front);

			noteStatsPaint(surface, frameNumber);
			dumpPaintedFrame(surface, frameNumber, pending.image);
			applyDecodedTile(entry.first, surface, pending.x, pending.y, pending.image);
		}
	}

	if (m_playoutTimer && nextDue != std::numeric_limits< qint64 >::max()) {
		m_playoutTimer->start(static_cast< int >(std::max< qint64 >(0, nextDue - now)));
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
	// And their keys, to hand any presentation of them over to the replacement.
	std::vector< std::uint64_t > replacedKeys;
	// The outgoing picture, to keep on screen until the replacement has one of its own.
	QImage heldFromReplaced;

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
					replacedKeys.push_back(it->first);

					if (heldFromReplaced.isNull()) {
						heldFromReplaced = it->second.canvas;
					}
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
				if (!recent->second.hasExpired()) {
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
		surface.pendingByPosition.clear();
		surface.refreshAfterOverload = false;
		surface.vp8Held.clear();
		surface.vp8GapSinceMsec     = 0;
		surface.vp8RequestedThrough = 0;
		surface.canvas = QImage();
		surface.vp8.reset();
#ifdef USE_H264
		// The same reasoning as surface.vp8 above, and it was missed here: an H264Decoder carries the
		// previous content's SPS, PPS and reference frames, and a half-assembled frame's parts belong to
		// a stream that no longer exists. A sender re-announcing one stream id as H.264, then VP9, then
		// H.264 again - which a codec setting change does - got the first incarnation's decoder back.
		surface.h264.reset();
		surface.h264Parts = H264FrameAssembler();
		surface.h264RecentPartCount = 1;
#endif
		++surface.decodeEpoch;
		resetPlayout(surface);
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

	// A share that starts while the preference is on is hidden from the outset.
	surface.hiddenByPreference = m_remoteVideoHidden;

	if (isNewSurface && resumeWatching && !replacedKeys.empty()) {
		// The viewer was watching the stream this one replaces, and may well have been watching it
		// somewhere other than the grid. Both presentations are bound to a surface key, and a key is a
		// sender and a stream id - so a sender who turns their phone, which forces a new stream id
		// every time they turn it, would have the fullscreen focus dropped and the picture-in-picture
		// window closed on each turn, putting the viewer back in front of the main window. The
		// presentation follows the stream instead.
		surface.awaitingReplacementPicture = true;
		surface.heldPicture                = heldFromReplaced;

		for (const std::uint64_t oldKey : replacedKeys) {
			repointPresentations(oldKey, key);
		}
	}

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
		it->second.pendingByPosition.clear();
		it->second.refreshAfterOverload = false;
		it->second.vp8Held.clear();
		it->second.vp8GapSinceMsec     = 0;
		it->second.vp8RequestedThrough = 0;
		it->second.hasDecodedFrame = false;
		it->second.lastFrameNumber = 0;
		// Nothing is going to arrive to end the transition, and the surface holds a cell as an ordinary
		// unwatched placeholder from here on.
		it->second.awaitingReplacementPicture = false;
		it->second.heldPicture                = QImage();
		++it->second.decodeEpoch;
		resetPlayout(it->second);
	}

	if (watching) {
		// A fresh grace period: whatever it was doing while unwatched - possibly nothing, for a long time
		// - must not count against it the instant someone actually starts watching. See
		// checkForStaleStreams().
		it->second.lastUnitMsec          = QDateTime::currentMSecsSinceEpoch();
		it->second.stallRefreshRequested = false;
	}

	// Nothing more arrives for an unwatched stream, so a window of its own would sit showing the last
	// frame it got. Back into the grid, where it becomes a preview placeholder like any other.
	if (!watching) {
		destroyPipWindow(it->first);
	}

	if (!watching && m_focus == FocusTarget::Surface && m_focusedSurfaceKey == it->first) {
		// Un-fullscreen whatever was just told to stop being watched, rather than leaving a placeholder
		// filling the whole grid.
		m_focus = FocusTarget::None;
	}

	emit watchToggled(senderSession, streamID, watching);
	relayout();
}

void VideoGrid::setWatchingAll(bool watching) {
	// Collected first: setWatching() relayouts, and nothing here should depend on what that does to the map.
	std::vector< std::pair< unsigned int, unsigned int > > streams;
	for (const auto &entry : m_surfaces) {
		streams.emplace_back(entry.second.senderSession, entry.second.streamID);
	}

	for (const auto &stream : streams) {
		setWatching(stream.first, stream.second, watching);
	}
}

static bool isScreenSourceKind(int sourceKind) {
	return sourceKind == MumbleProto::VideoState_SourceKind_Display
		   || sourceKind == MumbleProto::VideoState_SourceKind_Window;
}

bool VideoGrid::isScreenStream(unsigned int senderSession, unsigned int streamID) const {
	const auto it = m_surfaces.find(surfaceKey(senderSession, streamID));
	return it != m_surfaces.end() && isScreenSourceKind(it->second.sourceKind);
}

std::vector< std::pair< unsigned int, int > > VideoGrid::watchedStreams() const {
	std::vector< std::pair< unsigned int, int > > watched;
	for (const auto &entry : m_surfaces) {
		if (entry.second.watching) {
			watched.emplace_back(entry.second.senderSession, entry.second.sourceKind);
		}
	}
	return watched;
}

bool VideoGrid::isWatchingScreenOf(unsigned int senderSession) const {
	for (const auto &entry : m_surfaces) {
		const Surface &surface = entry.second;
		if (surface.senderSession == senderSession && surface.watching && isScreenSourceKind(surface.sourceKind)) {
			return true;
		}
	}

	return false;
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
									bool isKeyframe, unsigned int x, unsigned int y, const QByteArray &encodedTile,
									quint64 captureTimestampUsec) {
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

	++surface.stats.units;
	surface.stats.newestUnitFrame = std::max(surface.stats.newestUnitFrame, static_cast< quint64 >(frameNumber));

	// Before the watching check below, so that a frame whose first unit arrived just as watching began is
	// not measured as having taken however long the click took.
	if (m_statsTimer && captureTimestampUsec != 0) {
		Surface::Stats &stats = surface.stats;
		const qint64 nowMsec  = QDateTime::currentMSecsSinceEpoch();

		stats.captureUsecByFrame.emplace(frameNumber, captureTimestampUsec);

		while (stats.captureUsecByFrame.size() > MAX_LATENCY_FRAMES) {
			stats.captureUsecByFrame.erase(stats.captureUsecByFrame.begin());
		}

		if (stats.arrivalFloorWindowStartMsec == 0
			|| nowMsec - stats.arrivalFloorWindowStartMsec >= ARRIVAL_FLOOR_WINDOW_MSEC) {
			stats.previousArrivalFloorMsec    = stats.arrivalFloorMsec;
			stats.arrivalFloorMsec            = std::numeric_limits< qint64 >::max();
			stats.arrivalFloorWindowStartMsec = nowMsec;
		}

		stats.arrivalFloorMsec =
			std::min(stats.arrivalFloorMsec, nowMsec - static_cast< qint64 >(captureTimestampUsec / 1000));
	}

	if (!surface.watching) {
		// Unwatched: the owner has already sent (or is about to send) a VideoSubscribe withdrawing this,
		// so units are expected to stop arriving shortly, but a few in flight when the button was clicked
		// are ordinary, not a bug. Nothing to paint them into either way - the tile is a placeholder now.
		return;
	}

	// Capture times, for showing whole frames at the pace they were captured (see presentFrame()). Only with
	// threaded decoding, where frames come back from the decode thread asynchronously anyway.
	if (m_decodeWorker && captureTimestampUsec != 0 && surface.codec != MumbleProto::VideoState_Codec_TiledImage) {
		Surface::Playout &playout = surface.playout;
		const qint64 now          = QDateTime::currentMSecsSinceEpoch();

		playout.captureUsec[frameNumber] = captureTimestampUsec;
		while (playout.captureUsec.size() > 2 * MAX_PLAYOUT_QUEUE + MAX_VP8_HELD) {
			playout.captureUsec.erase(playout.captureUsec.begin());
		}

		if (playout.windowStartMsec == 0 || now - playout.windowStartMsec >= 10000) {
			playout.previousMin     = playout.windowMin;
			playout.windowMin       = std::numeric_limits< qint64 >::max();
			playout.windowStartMsec = now;
		}
		playout.windowMin = std::min(playout.windowMin, now - static_cast< qint64 >(captureTimestampUsec / 1000));
	}

	// VP9 (screen shares) goes through the same whole-frame path as VP8 (cameras): frame ordering, holding
	// frames across a gap while it is retransmitted, and keyframe requests are the same for both.
	if (surface.codec == MumbleProto::VideoState_Codec_VP8 || surface.codec == MumbleProto::VideoState_Codec_VP9) {
		// A whole frame covers the surface, so its position is always the origin - every encoder here sets
		// x and y to zero for these codecs. A peer sending anything else gets its frame painted into an
		// oversized canvas instead of becoming the canvas: at the surface bound that is 33 MB held and a
		// full-frame copy on the GUI thread for every frame of the share, from one field nobody validated.
		// Refused rather than clamped, because a legitimate sender never sends it.
		if (x != 0 || y != 0) {
			return;
		}

		onVp8UnitReceived(existing->first, surface, frameNumber, isKeyframe, x, y, encodedTile);
		return;
	}

#ifdef USE_H264
	if (surface.codec == MumbleProto::VideoState_Codec_H264) {
		// x and y are this part's index and the frame's part count (see Mumble.proto).
		onH264PartReceived(existing->first, surface, frameNumber, isKeyframe, x, y, encodedTile);
		return;
	}
#endif

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
			// Behind. Dropping is how this catches up; the refresh that repaints what was dropped is asked
			// for once, when the queue has drained (see applyReadyTiles()). Asking now - for a keyframe, every
			// tile at once - is what used to turn a viewer that had fallen behind into one that never caught
			// up, and made the sender re-encode whole frames for everybody while it happened.
			++surface.stats.keyframeRequestsOverload;
			surface.refreshAfterOverload = true;
			return;
		}

		auto pending         = std::make_shared< PendingTile >();
		pending->frameNumber = frameNumber;
		pending->x           = x;
		pending->y           = y;

		// Whichever of this tile and one still waiting at the same position belongs to the older frame is
		// out of date: let its worker skip it. By frame number, not arrival - a late tile from an earlier
		// frame can arrive after a newer one, and must lose to it.
		std::shared_ptr< PendingTile > &newestHere =
			surface.pendingByPosition[(static_cast< std::uint64_t >(x) << 32) | y];
		if (!newestHere) {
			newestHere = pending;
		} else if (pending->frameNumber >= newestHere->frameNumber) {
			newestHere->superseded.store(true, std::memory_order_relaxed);
			newestHere = pending;
		} else {
			pending->superseded.store(true, std::memory_order_relaxed);
		}

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

		watcher->setFuture(QtConcurrent::run([pending, encodedTile]() {
			return pending->superseded.load(std::memory_order_relaxed) ? QImage() : decodeJpegTile(encodedTile);
		}));

		return;
	}

	// Any other codec is one this build cannot decode (CODEC_UNKNOWN, or one a newer build added). Its units
	// are dropped; asking its sender for keyframes would be a request nothing here could ever satisfy.
}

void VideoGrid::onVp8UnitReceived(std::uint64_t key, Surface &surface, quint64 frameNumber, bool isKeyframe,
								  unsigned int x, unsigned int y, const QByteArray &payload) {
	const unsigned int senderSession = surface.senderSession;
	const unsigned int streamID      = surface.streamID;

	// VP8 inter-frames reference their predecessor, and decoding one whose reference is missing does not
	// fail - it returns a plausible-looking corrupted image, classically green. So frames are only ever
	// decoded in order: a frame that arrives after a gap is held while the missing ones are re-sent.
	if (!surface.hasDecodedFrame) {
		if (isKeyframe) {
			if (decodeVp8Unit(key, surface, frameNumber, x, y, payload)) {
				drainHeldVp8(key, surface);
				noteVp8Progress(surface);
			}
			return;
		}

		// An inter-frame before the stream's first keyframe has been decoded - typically because that
		// keyframe, many times larger (a VP9 screen keyframe is around a hundred fragments), is still being
		// reassembled or retransmitted while the small frames after it complete. These used to be decoded
		// on the spot, which fails, and so discarded - for good, since the reassembler will not rebuild a unit
		// it has already completed, and asking for them again gets nothing. Every stream then broke at the
		// frame after its first keyframe. Held instead, to be decoded in order once the keyframe is.
		Surface::HeldVp8Unit early;
		early.payload                = payload;
		early.x                      = x;
		early.y                      = y;
		surface.vp8Held[frameNumber] = early;
		while (surface.vp8Held.size() > MAX_VP8_HELD) {
			surface.vp8Held.erase(surface.vp8Held.begin());
		}

		// No keyframe yet: ask now and then, in case the request that should have produced one was lost.
		if (++surface.unitsWhileAwaiting >= KEYFRAME_REREQUEST_AFTER_UNITS) {
			surface.unitsWhileAwaiting = 0;

			++surface.stats.keyframeRequestsFailure;
			emit keyframeNeeded(senderSession, streamID);
		}
		return;
	}

	if (frameNumber <= surface.lastFrameNumber) {
		// A stale unit that reassembled after its successors - a keyframe included. Decoding it would rewind
		// the decoder's reference state (and, for a keyframe, lastFrameNumber itself).
		return;
	}

	if (surface.awaitingKeyframe && !isKeyframe) {
		// Frozen: the last good picture stays up, which reads as a pause instead of a burst of green.
		//
		// Held all the same, for the same reason as before the first keyframe above: the keyframe asked for
		// is large, often completes after the frames that follow it, and those are decoded after it, in order,
		// rather than discarded and lost for good. Held frames older than the keyframe are dropped when it
		// arrives (drainHeldVp8()).
		Surface::HeldVp8Unit waiting;
		waiting.payload              = payload;
		waiting.x                    = x;
		waiting.y                    = y;
		surface.vp8Held[frameNumber] = waiting;
		while (surface.vp8Held.size() > MAX_VP8_HELD) {
			surface.vp8Held.erase(surface.vp8Held.begin());
		}

		// Re-ask occasionally in case the request was lost in the same loss burst that caused the gap.
		if (++surface.unitsWhileAwaiting >= KEYFRAME_REREQUEST_AFTER_UNITS) {
			surface.unitsWhileAwaiting = 0;

			++surface.stats.keyframeRequestsLoss;
			emit keyframeNeeded(senderSession, streamID);
		}

		return;
	}

	if (isKeyframe || frameNumber == surface.lastFrameNumber + 1) {
		if (!isKeyframe && !surface.vp8Held.empty()) {
			// The frame a gap was waiting for: retransmission works for this stream.
			++surface.vp8GapsRecovered;
		}
		if (decodeVp8Unit(key, surface, frameNumber, x, y, payload)) {
			drainHeldVp8(key, surface);
			noteVp8Progress(surface);
		}
		return;
	}

	// A gap: the frames between were lost, or are late. Hold this one, ask for each missing frame, and carry
	// on in order as soon as they arrive - a retransmission costs about a round trip, where waiting for a
	// keyframe cost seconds.
	//
	// There used to be a shortcut here that skipped holding once a stream had abandoned three gaps without
	// ever recovering one, on the theory that its server predated retransmission. Under steady loss two or
	// three early gaps can be lost for good on a server that does retransmit, and the shortcut then turned
	// every later loss into a keyframe for the rest of the stream: measured, one run in four of a VP9 screen
	// share at 2% loss stuck at 1 fps that way. Against a server that really cannot retransmit, holding
	// costs VP8_HOLD_MSEC before the keyframe request, which is the better trade.
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	// Frames held while waiting for a keyframe never started the clock, so a gap left after that keyframe
	// starts it here too.
	if (surface.vp8Held.empty() || surface.vp8GapSinceMsec == 0) {
		surface.vp8GapSinceMsec = now;
	}

	Surface::HeldVp8Unit held;
	held.payload                 = payload;
	held.x                       = x;
	held.y                       = y;
	surface.vp8Held[frameNumber] = held;

	// Whole frames are asked for only once the gap is VP8_WHOLE_FRAME_REQUEST_MSEC old. Most frames
	// "missing" here are only partly arrived, and the reassembler already asks for exactly the fragments
	// they lack; asking for the whole frame as well re-sent it in full - measured at about two thirds on
	// top of a 3 Mbit/s screen share at 2% loss. A frame of which nothing arrived at all is still asked for.
	if (now - surface.vp8GapSinceMsec >= VP8_WHOLE_FRAME_REQUEST_MSEC) {
		const quint64 firstMissing = std::max(surface.lastFrameNumber + 1, surface.vp8RequestedThrough + 1);
		for (quint64 missing = firstMissing; missing < frameNumber && missing <= surface.lastFrameNumber + MAX_VP8_HELD;
			 ++missing) {
			if (surface.vp8Held.find(missing) == surface.vp8Held.end()) {
				requestRetransmit(surface, missing);
			}
		}
		surface.vp8RequestedThrough = std::max(surface.vp8RequestedThrough, frameNumber - 1);
	}

	if (surface.vp8Held.size() > MAX_VP8_HELD || now - surface.vp8GapSinceMsec > VP8_HOLD_MSEC) {
		// Not coming back in time: fall back to freezing until a keyframe, as before retransmission.
		qWarning("VideoGrid: stream %u/%u lost frame %llu for good, frozen until a keyframe arrives", senderSession,
				 streamID, static_cast< unsigned long long >(surface.lastFrameNumber + 1));

		surface.vp8Held.clear();
		surface.vp8GapSinceMsec    = 0;
		surface.awaitingKeyframe   = true;
		surface.unitsWhileAwaiting = 0;
		++surface.vp8GapsAbandoned;

		++surface.stats.keyframeRequestsLoss;
		emit keyframeNeeded(senderSession, streamID);
	}
}

bool VideoGrid::decodeVp8Unit(std::uint64_t key, Surface &surface, quint64 frameNumber, unsigned int x, unsigned int y,
							  const QByteArray &payload) {
	if (m_decodeWorker) {
		// Threaded: the frame goes to the decode thread in order, and counts as decoded for the ordering
		// logic here. If it turns out undecodable, the failures that follow ask for a keyframe (see
		// onFrameDecoded()), as they would have done here.
		if (!prepareDecoder(surface)) {
			noteDecodeFailure(surface);
			return false;
		}

		surface.lastFrameNumber    = frameNumber;
		surface.hasDecodedFrame    = true;
		surface.awaitingKeyframe   = false;
		surface.unitsWhileAwaiting = 0;

		const std::shared_ptr< std::atomic< int > > inFlight = surface.decodesInFlight;
		inFlight->fetch_add(1);

		const std::shared_ptr< VpxDecoder > vpx = surface.vp8;
#ifdef USE_H264
		const std::shared_ptr< H264Decoder > h264 =
			surface.codec == MumbleProto::VideoState_Codec_H264 ? surface.h264 : nullptr;
#endif
		const std::uint64_t epoch = surface.decodeEpoch;
		const QPointer< VideoGrid > self(this);

		QMetaObject::invokeMethod(
			m_decodeWorker,
			[=]() {
				// Every frame is converted, to be shown at its own time (see presentFrame()), unless the decode
				// thread has fallen well behind: then only the frame at the end of the queue is worth it.
				const bool latest = inFlight->load() <= 8;
				QImage image;
				bool decoded = false;
#ifdef USE_H264
				if (h264) {
					decoded = h264->decode(payload, latest ? &image : nullptr);
				} else
#endif
				{
					const auto *bytes = reinterpret_cast< const Mumble::Protocol::byte * >(payload.constData());
					decoded           = vpx->decode(std::vector< Mumble::Protocol::byte >(
                                              bytes, bytes + static_cast< std::size_t >(payload.size())),
                                          latest ? &image : nullptr);
				}
				inFlight->fetch_sub(1);

				QMetaObject::invokeMethod(
					self.data(),
					[=]() {
						if (self) {
							self->onFrameDecoded(key, epoch, frameNumber, x, y, decoded, image);
						}
					},
					Qt::QueuedConnection);
			},
			Qt::QueuedConnection);

		return true;
	}

	const QImage tile = decodeUnit(surface, payload);

	if (tile.isNull()) {
		noteDecodeFailure(surface);
		return false;
	}

	surface.consecutiveFailures = 0;
	surface.lastFrameNumber     = frameNumber;
	surface.hasDecodedFrame     = true;
	surface.awaitingKeyframe    = false;
	surface.unitsWhileAwaiting  = 0;

	noteStatsPaint(surface, frameNumber);
	dumpPaintedFrame(surface, frameNumber, tile);
	applyDecodedTile(key, surface, x, y, tile);

	return true;
}

void VideoGrid::drainHeldVp8(std::uint64_t key, Surface &surface) {
	while (!surface.vp8Held.empty()) {
		const auto next = surface.vp8Held.begin();

		if (next->first <= surface.lastFrameNumber) {
			// Overtaken - by a keyframe, typically.
			surface.vp8Held.erase(next);
			continue;
		}

		if (next->first != surface.lastFrameNumber + 1) {
			return;
		}

		const Surface::HeldVp8Unit held = next->second;
		const quint64 frameNumber       = next->first;
		surface.vp8Held.erase(next);

		if (!decodeVp8Unit(key, surface, frameNumber, held.x, held.y, held.payload)) {
			return;
		}
	}

	surface.vp8GapSinceMsec = 0;
}

void VideoGrid::noteVp8Progress(Surface &surface) {
	// The hold deadline is for the gap the picture is stuck on now, not for the first gap of a run of them.
	// Under steady loss a new gap often opens before the last one is fully drained; timing from the first
	// one abandoned streams that were recovering frame after frame.
	if (!surface.vp8Held.empty()) {
		surface.vp8GapSinceMsec = QDateTime::currentMSecsSinceEpoch();
	}
}

void VideoGrid::requestRetransmit(Surface &surface, quint64 frameNumber) {
#ifdef USE_H264
	if (surface.codec == MumbleProto::VideoState_Codec_H264) {
		QList< unsigned int > units;

		for (const unsigned int part : surface.h264Parts.missingParts(frameNumber)) {
			units.append(part);
		}

		if (units.isEmpty()) {
			// Nothing of it arrived, so how many parts it had is unknown. Asking for a few more than the
			// last frame had costs a sender nothing for units it never sent: it only re-sends what it has.
			const unsigned int guess = std::min(surface.h264RecentPartCount + 2, H264Encoder::MAX_UNITS_PER_FRAME);
			for (unsigned int i = 0; i < guess; ++i) {
				units.append(i);
			}
		}

		emit unitsRetransmitNeeded(surface.senderSession, surface.streamID, frameNumber, units);
		return;
	}
#endif

	emit retransmitNeeded(surface.senderSession, surface.streamID, frameNumber);
}

#ifdef USE_H264
void VideoGrid::onH264PartReceived(std::uint64_t key, Surface &surface, quint64 frameNumber, bool isKeyframe,
								   unsigned int index, unsigned int count, const QByteArray &part) {
	H264FrameAssembler::Complete frame;

	if (!surface.h264Parts.add(frameNumber, isKeyframe, index, count, part, frame)) {
		return;
	}

	surface.h264RecentPartCount = count;

	onVp8UnitReceived(key, surface, frame.frameNumber, frame.isKeyframe, 0, 0, frame.accessUnit);
}
#endif

void VideoGrid::applyReadyTiles(unsigned int senderSession, unsigned int streamID) {
	// Workers may finish in any order. Paint in arrival order, or an old JPEG can overwrite a newer
	// one. Re-look up after each paint because UI signals may remove or replace this surface.
	for (;;) {
		const auto it = m_surfaces.find(surfaceKey(senderSession, streamID));
		if (it == m_surfaces.end()) {
			return;
		}

		Surface &surface = it->second;

		if (surface.pendingTiles.empty()) {
			if (surface.refreshAfterOverload) {
				// Caught up after dropping tiles: now one refresh repaints whatever was dropped.
				surface.refreshAfterOverload = false;
				emit keyframeNeeded(senderSession, streamID);
			}
			return;
		}

		if (!surface.pendingTiles.front()->ready) {
			return;
		}

		const std::shared_ptr< PendingTile > tile = surface.pendingTiles.front();
		surface.pendingTiles.pop_front();

		const auto position = surface.pendingByPosition.find((static_cast< std::uint64_t >(tile->x) << 32) | tile->y);
		if (position != surface.pendingByPosition.end() && position->second == tile) {
			surface.pendingByPosition.erase(position);
		}

		if (tile->superseded.load(std::memory_order_relaxed)) {
			// A newer tile for the same spot is queued behind it; this one was (usually) never decoded.
			continue;
		}

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

			++surface.stats.keyframeRequestsFailure;
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

	noteStatsPaint(surface, frameNumber);
	dumpPaintedFrame(surface, frameNumber, tile);
	applyDecodedTile(existing->first, surface, x, y, tile);
}

void VideoGrid::applyDecodedTile(std::uint64_t key, Surface &surface, unsigned int x, unsigned int y,
								 const QImage &tile) {
	// A surface exists from the announcement onwards but holds no picture until now, so this is what
	// makes the sender count - and with it the video panel - appear.
	const bool wasBlank    = surface.canvas.isNull();
	const QSize sizeBefore = surface.canvas.size();

	if (x == 0 && y == 0
		&& (isWholeFrameCodec(surface.codec)
			|| (!wasBlank && tile.size() == sizeBefore && tile.format() == surface.canvas.format()))) {
		// A whole frame (VP8, VP9, H.264) is the picture, so it is taken as it is, sharing the decoded
		// image's pixels, instead of painted over the old one - which copied every pixel of every frame on
		// the GUI thread, about 8 MB a frame at 1080p.
		//
		// Including when its shape changes, which is what a sender turning a phone does: these codecs
		// carry their own dimensions on every keyframe and libvpx reinitialises on one, so the frame that
		// comes out is simply the new picture. Taking it through growToFit() instead - which only ever
		// grows - left a strip of the old, wider picture alongside the new one for the rest of the share.
		// A tile of a larger picture is a different matter, and still goes below.
		surface.canvas = tile;
	} else {
		if (!growToFit(surface.canvas, static_cast< int >(x), static_cast< int >(y), tile.width(), tile.height())) {
			return;
		}

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

	// Whatever this surface replaced has been superseded by a frame of its own, so it is no longer in
	// the transition repointPresentations() held its presentation open for, and the outgoing picture it
	// was standing in with is not needed - nor wanted, at the wrong shape.
	surface.awaitingReplacementPicture = false;
	surface.heldPicture                = QImage();

	if (wasBlank) {
		emit senderCountChanged(tileCount());
		relayout();

		return;
	}

	const QRect changedRect(static_cast< int >(x), static_cast< int >(y), tile.width(), tile.height());

	if (m_focus == FocusTarget::Surface && m_focusedSurfaceKey == key) {
		// Just the rectangle this tile covers, in canvas pixels: the fullscreen window maps that onto the
		// monitor and repaints only that patch. Repainting the whole monitor for every 128-pixel tile of
		// a screen share was, alongside the canvas copy described on FullscreenVideoWindow, what made
		// watching one fullscreen so expensive.
		notifyFullscreenContentChanged(changedRect);
	}

	// And the same patch to this surface's own picture-in-picture window, if it has one.
	notifyPipContentChanged(key, changedRect);

	// Likewise only the patch of this tile's cell the tile covers, not the whole cell or grid: paintEvent()
	// is clipped to the region it is asked for, so everything else is neither rescaled nor handed to the
	// compositor again. Repainting the whole cell for every 128-pixel tile meant a viewer's window sent the
	// compositor a full-cell shared-memory buffer at the display's refresh rate - on a 3440x1440 desktop,
	// about 260 MB/s for the compositor to upload, which kept the GPU busy enough to lag the whole desktop.
	const int slot = slotForSurface(key);

	if (slot < 0) {
		scheduleGridRepaint(rect());
	} else if (surface.canvas.size() != sizeBefore) {
		// The picture grew, so the letterboxed target moved: none of what was painted is still in place.
		scheduleGridRepaint(cellRect(currentLayout(), slot));
	} else {
		markTileDirty(cellRect(currentLayout(), slot), surface.canvas.size(),
					  QRect(static_cast< int >(x), static_cast< int >(y), tile.width(), tile.height()));
	}
}

QRect VideoGrid::pictureRect(const QRect &cell, const QSize &imageSize) {
	const QSize scaled = imageSize.scaled(cell.size(), Qt::KeepAspectRatio);

	return QRect(cell.x() + (cell.width() - scaled.width()) / 2, cell.y() + (cell.height() - scaled.height()) / 2,
				 scaled.width(), scaled.height());
}

void VideoGrid::markTileDirty(const QRect &cell, const QSize &imageSize, const QRect &tileRect) {
	const QRect target = pictureRect(cell, imageSize);

	if (imageSize.isEmpty() || target.isEmpty()) {
		scheduleGridRepaint(cell);

		return;
	}

	const double scaleX = static_cast< double >(target.width()) / imageSize.width();
	const double scaleY = static_cast< double >(target.height()) / imageSize.height();

	// Padded by a couple of pixels either side, as FullscreenVideoWindow::flushRepaint() does: the scale is
	// fractional, and a patch has to be repainted a little wider than rounding could have moved its edge.
	const QRect dirty(static_cast< int >(std::floor(target.x() + tileRect.x() * scaleX)) - 2,
					  static_cast< int >(std::floor(target.y() + tileRect.y() * scaleY)) - 2,
					  static_cast< int >(std::ceil(tileRect.width() * scaleX)) + 4,
					  static_cast< int >(std::ceil(tileRect.height() * scaleY)) + 4);

	scheduleGridRepaint(dirty.intersected(cell));
}

void VideoGrid::scheduleGridRepaint(const QRect &dirty) {
	if (dirty.isEmpty()) {
		return;
	}

	m_gridDirty |= dirty;

	if (!m_gridRepaintTimer->isActive()) {
		m_gridRepaintTimer->start();
	}
}

void VideoGrid::flushGridRepaint() {
	if (m_gridDirty.isEmpty()) {
		return;
	}

	update(m_gridDirty);
	m_gridDirty = QRegion();
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
	scheduleGridRepaint(cellRect(currentLayout(), 0));
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
	scheduleGridRepaint(cellRect(currentLayout(), m_selfCameraFrame.isNull() ? 0 : 1));
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
		m_recentlyWatched[{ senderSession, it->second.sourceKind }] = QDeadlineTimer(RESUME_WATCH_GRACE_MSEC);
	}

	// Before the surface goes: a picture-in-picture window whose stream has ended has nothing left to
	// show, and leaving it open would be the frozen-last-frame problem with an extra window around it.
	destroyPipWindow(it->first);

	m_surfaces.erase(it);
	emit senderCountChanged(tileCount());
	relayout();
}

void VideoGrid::removeSender(unsigned int senderSession) {
	bool removedAny = false;

	for (auto it = m_surfaces.begin(); it != m_surfaces.end();) {
		if (it->second.senderSession == senderSession) {
			destroyPipWindow(it->first);

			it         = m_surfaces.erase(it);
			removedAny = true;
		} else {
			++it;
		}
	}

	m_senderNames.erase(senderSession);
	m_shareVolumePercent.erase(senderSession);

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

	// Every picture-in-picture window belongs to a surface that is about to be gone.
	while (!m_pipWindows.empty()) {
		destroyPipWindow(m_pipWindows.begin()->first);
	}

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
	m_shareVolumePercent.clear();
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

/// The translucent strip along the bottom of a tile that holds its buttons and volume slider.
///
/// The dark background is scoped to the strip itself by object name; applied as a bare property, a style
/// sheet cascades into every child widget as well.
///
/// The slider is styled completely rather than left to the theme, after Discord's stream volume: a thin
/// rounded track, the filled part in Discord's blurple, a round white knob. Every property the bundled
/// themes set on sliders is overridden here, because they are not meant for a slider on top of video: the
/// Dark and Lite themes give every QSlider 30px of margin on each side (which squeezed a 110px slider to a
/// 50px track) and a 4.5em-wide bordered grey handle (which looked like a blob).
static QWidget *makeTileControlStrip(QWidget *parent) {
	auto *strip = new QWidget(parent);
	strip->setObjectName(QStringLiteral("tileControls"));
	strip->setAttribute(Qt::WA_StyledBackground, true);
	strip->setStyleSheet(QStringLiteral(
		"#tileControls { background-color: rgba(0, 0, 0, 140); }"
		"#tileControls QLabel, #tileControls QToolButton { color: white; background: transparent; }"
		"#tileControls QSlider { background: transparent; margin: 0px; padding: 0px; }"
		"#tileControls QSlider::groove:horizontal {"
		"  height: 4px; margin: 0px; border: none; border-radius: 2px; background: rgba(255, 255, 255, 70); }"
		"#tileControls QSlider::sub-page:horizontal {"
		"  height: 4px; border: none; border-radius: 2px; background: #5865F2; }"
		"#tileControls QSlider::add-page:horizontal {"
		"  height: 4px; border: none; border-radius: 2px; background: rgba(255, 255, 255, 70); }"
		"#tileControls QSlider::handle:horizontal {"
		"  width: 14px; height: 14px; margin: -5px 0px; border: none; border-radius: 7px; background: white; }"
		"#tileControls QSlider::handle:horizontal:hover,"
		"#tileControls QSlider::handle:horizontal:focus,"
		"#tileControls QSlider::handle:horizontal:pressed { background: #e3e5ff; border: none; }"));
	return strip;
}

std::unique_ptr< VideoGrid::TileControlBar > VideoGrid::makeOwnControlBar(FocusTarget target) {
	auto controls = std::make_unique< TileControlBar >();

	controls->bar = makeTileControlStrip(this);

	auto *layout = new QHBoxLayout(controls->bar);
	layout->setContentsMargins(4, 2, 4, 2);
	layout->setSpacing(4);

	// Who is watching: the count, and on click the names - with the volume each has you at, for a share.
	controls->viewersButton = new QToolButton(controls->bar);
	controls->viewersButton->setAutoRaise(true);
	controls->viewersButton->setFocusPolicy(Qt::NoFocus);
	controls->viewersButton->setPopupMode(QToolButton::InstantPopup);
	controls->viewersButton->setMenu(new QMenu(controls->viewersButton));
	// Its text is an eye and a digit, which reads as nothing at all. refreshViewersButton() keeps the
	// tooltip - the names and the level each one hears - in step with the menu.
	controls->viewersButton->setAccessibleName(tr("Who is watching"));
	layout->addWidget(controls->viewersButton);
	refreshViewersButton(controls.get(),
						 target == FocusTarget::SelfScreen ? m_ownScreenViewers : m_ownCameraViewers);

	layout->addStretch();

	// No picture-in-picture button on your own tiles. They are previews of what you are sending, held in
	// reserved cells of their own (see the class comment), with no surface in m_surfaces to move out - so
	// a button here would have to mean something different from the identical one on everybody else's
	// tile, which is worse than not offering it.
	controls->fullscreenButton = new QToolButton(controls->bar);
	controls->fullscreenButton->setAutoRaise(true);
	controls->fullscreenButton->setText(QStringLiteral("⛶"));
	controls->fullscreenButton->setToolTip(tr("Fullscreen"));
	controls->fullscreenButton->setAccessibleName(tr("Fullscreen"));
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

	controls->bar = makeTileControlStrip(this);

	auto *layout = new QHBoxLayout(controls->bar);
	layout->setContentsMargins(4, 2, 4, 2);
	layout->setSpacing(4);

	// A camera has no audio of its own to control - voice is muted and adjusted the same way it always
	// has been, from the user list. Only a screen share carries a second, independent audio stream worth
	// a slider of its own.
	if (hasAudio) {
		// Laid out like Discord's stream volume: a speaker button that mutes and unmutes, the slider, and the
		// level as a percentage.
		const int defaultPercent = static_cast< int >(AudioOutputScreenShare::DEFAULT_VOLUME * 100.0f);
		const auto remembered    = m_shareVolumePercent.find(senderSession);
		const int percent        = remembered != m_shareVolumePercent.end() ? remembered->second : defaultPercent;

		auto *muteButton = new QToolButton(controls->bar);
		muteButton->setAutoRaise(true);
		muteButton->setIconSize(QSize(16, 16));
		muteButton->setFocusPolicy(Qt::NoFocus);
		muteButton->setAccessibleName(tr("Mute this stream"));
		layout->addWidget(muteButton);

		auto *slider = new StreamVolumeSlider(controls->bar);
		slider->setRange(0, static_cast< int >(AudioOutputScreenShare::MAX_VOLUME * 100.0f));
		slider->setSingleStep(5);
		slider->setPageStep(25);
		slider->setFixedWidth(110);
		slider->setFocusPolicy(Qt::NoFocus);
		slider->setToolTip(tr("Stream volume"));
		slider->setAccessibleName(tr("Stream volume"));
		layout->addWidget(slider);

		auto *label = new QLabel(controls->bar);
		label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		// Wide enough for "200%", so the slider does not shift while dragging.
		label->setFixedWidth(label->fontMetrics().horizontalAdvance(QStringLiteral("200%")) + 4);
		layout->addWidget(label);

		controls->muteButton   = muteButton;
		controls->volumeSlider = slider;
		controls->volumeLabel  = label;

		// Where unmuting returns to. Shared by the two handlers below rather than kept in the
		// TileControlBar, which can be destroyed while these widgets are still waiting on deleteLater().
		auto volumeBeforeMute = std::make_shared< int >(percent > 0 ? percent : defaultPercent);

		// Which way the speaker button is currently drawn, so that a drag through a hundred levels sets the
		// icon at most once - when it crosses zero - instead of on every value. Shared the same way
		// volumeBeforeMute is, and for the same reason.
		auto shownAsMuted = std::make_shared< std::optional< bool > >();

		const auto showLevel = [this, muteButton, label, slider, shownAsMuted](int value) {
			const QString level = tr("%1%").arg(value);
			label->setText(level);
			slider->setLevelText(level);

			const bool muted = value == 0;

			if (*shownAsMuted != muted) {
				*shownAsMuted = muted;
				muteButton->setIcon(volumeIcon(style(), muted));
				muteButton->setToolTip(muted ? tr("Unmute stream") : tr("Mute stream"));
			}
		};

		slider->setValue(percent);
		showLevel(percent);

		connect(slider, &QSlider::valueChanged, slider, [this, senderSession, volumeBeforeMute, showLevel](int value) {
			m_shareVolumePercent[senderSession] = value;
			if (value > 0) {
				*volumeBeforeMute = value;
			}
			showLevel(value);
			emit volumeChanged(senderSession, static_cast< float >(value) / 100.0f);

			// The fullscreen bar shows the same level, so it follows.
			if (m_fullscreenWindow) {
				static_cast< FullscreenVideoWindow * >(m_fullscreenWindow)->refreshVolume();
			}
		});

		connect(muteButton, &QToolButton::clicked, slider,
				[slider, volumeBeforeMute]() { slider->setValue(slider->value() == 0 ? *volumeBeforeMute : 0); });

		// updateHoveredBarImpl() leaves the bars alone while this is held down (see
		// aVolumeSliderIsBeingDragged()), so whatever it wanted to do is done once the drag ends - the
		// pointer may well have left the tile by then.
		connect(slider, &QSlider::sliderReleased, this, [this]() { scheduleHoveredBarUpdate(); });

		// The audio buffer for this share may already exist at another level - a share restarted after
		// the slider was moved - so say where the slider stands rather than assume the buffer agrees.
		emit volumeChanged(senderSession, static_cast< float >(percent) / 100.0f);
	}

	layout->addStretch();

	const std::uint64_t key = surfaceKey(senderSession, streamID);

	// Beside the fullscreen button rather than replacing it: watching somebody's screen share while
	// getting on with something else is a different thing from filling the monitor with it, and both are
	// worth one press.
	controls->popOutButton = new QToolButton(controls->bar);
	controls->popOutButton->setAutoRaise(true);
	controls->popOutButton->setText(QStringLiteral("❐"));
	controls->popOutButton->setToolTip(tr("Show in its own window, outside the grid"));
	controls->popOutButton->setAccessibleName(tr("Show in its own window"));
	layout->addWidget(controls->popOutButton);

	connect(controls->popOutButton, &QToolButton::clicked, this, [this, senderSession, streamID]() {
		if (isPoppedOut(senderSession, streamID)) {
			popInSurface(senderSession, streamID);
		} else {
			popOutSurface(senderSession, streamID);
		}
	});

	controls->fullscreenButton = new QToolButton(controls->bar);
	controls->fullscreenButton->setAutoRaise(true);
	controls->fullscreenButton->setText(QStringLiteral("⛶"));
	controls->fullscreenButton->setToolTip(tr("Fullscreen"));
	controls->fullscreenButton->setAccessibleName(tr("Fullscreen"));
	layout->addWidget(controls->fullscreenButton);

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

		if (!occupiesCell(surface)) {
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

bool VideoGrid::aVolumeSliderIsBeingDragged() const {
	for (const auto &entry : m_remoteControls) {
		if (entry.second->volumeSlider && entry.second->volumeSlider->isSliderDown()) {
			return true;
		}
	}

	return false;
}

void VideoGrid::updateHoveredBarImpl() {
	// Hiding the bar a slider is being dragged on takes the slider out from under the pointer and ends the
	// drag where it happens to be. That is reachable without the pointer going anywhere: anything that
	// changes the layout - somebody else starting a share, a tile ending, the dock being resized - comes
	// through relayout() and so through here. Asked of the sliders rather than remembered in a flag, which
	// a bar destroyed mid-drag would leave stuck on.
	//
	// Nothing below has run, so m_hoveredSlot still describes what is on screen; the sliderReleased
	// connection in makeRemoteControlBar() asks again once the drag is over. The cost is that a bar
	// created while a drag is in progress - somebody starting a share at that moment - stays visible
	// until the drag ends, which is a second of a bar being shown rather than a drag going somewhere
	// the user did not aim it.
	if (aVolumeSliderIsBeingDragged()) {
		return;
	}

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
		if (!occupiesCell(entry.second)) {
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
		m_fullscreenWindow = new FullscreenVideoWindow(
			this,
			[this](QImage &image, QString &label) {
				if (m_focus == FocusTarget::SelfCamera) {
					image = m_selfCameraFrame;
					label = ownTileLabel(false);
				} else if (m_focus == FocusTarget::SelfScreen) {
					image = m_selfScreenFrame;
					label = ownTileLabel(true);
				} else if (m_focus == FocusTarget::Surface) {
					const auto it = m_surfaces.find(m_focusedSurfaceKey);

					if (it != m_surfaces.end()) {
						image = displayImage(it->second);
						label = labelForSurface(it->second);
					}
				}
			},
			[this]() {
				FullscreenVideoWindow::VolumeState state;
				if (m_focus != FocusTarget::Surface) {
					return state;
				}

				const auto it = m_surfaces.find(m_focusedSurfaceKey);
				// Only a stream with audio of its own has a volume here - a screen share; a camera's sound is
				// its sender's voice, adjusted from the user list as always.
				if (it == m_surfaces.end() || it->second.sourceKind == MumbleProto::VideoState_SourceKind_Camera) {
					return state;
				}

				const auto level = m_shareVolumePercent.find(it->second.senderSession);
				state.available  = true;
				state.percent    = level != m_shareVolumePercent.end()
									   ? level->second
									   : static_cast< int >(AudioOutputScreenShare::DEFAULT_VOLUME * 100.0f);
				return state;
			},
			[this](int percent) {
				const auto it = m_surfaces.find(m_focusedSurfaceKey);
				if (m_focus == FocusTarget::Surface && it != m_surfaces.end()) {
					setShareVolumePercent(it->second.senderSession, percent);
				}
			});
	}

	// A relayout() is the "something structural changed" path - a different tile focused, a stream
	// re-announced, the picture appearing for the first time - so the whole window is repainted. The
	// per-tile path, applyDecodedTile(), goes through notifyFullscreenContentChanged() with just the
	// changed rectangle instead.
	static_cast< FullscreenVideoWindow * >(m_fullscreenWindow)->contentReplaced();

	static_cast< FullscreenVideoWindow * >(m_fullscreenWindow)->setPresentation(false);
}

void VideoGrid::setOverlaySupport(OverlayItemFactory factory, OverlayItemRefresher refresher) {
	m_overlayFactory   = std::move(factory);
	m_overlayRefresher = std::move(refresher);
}

QGraphicsItem *VideoGrid::buildOverlayItem() {
	return m_overlayFactory ? m_overlayFactory() : nullptr;
}

void VideoGrid::refreshOverlayItem(QGraphicsItem *item, const QSizeF &size) {
	if (item && m_overlayRefresher) {
		m_overlayRefresher(item, size);
	}
}

void VideoGrid::setVideoOverlayEnabled(bool enabled) {
	if (m_videoOverlayEnabled == enabled) {
		return;
	}

	m_videoOverlayEnabled = enabled;

	if (m_fullscreenWindow) {
		static_cast< FullscreenVideoWindow * >(m_fullscreenWindow)->setOverlayEnabled(enabled);
	}
}

bool VideoGrid::isPoppedOut(unsigned int senderSession, unsigned int streamID) const {
	return m_pipWindows.count(surfaceKey(senderSession, streamID)) > 0;
}

void VideoGrid::popOutSurface(unsigned int senderSession, unsigned int streamID) {
	const std::uint64_t key = surfaceKey(senderSession, streamID);
	const auto surfaceIt    = m_surfaces.find(key);

	if (surfaceIt == m_surfaces.end() || m_pipWindows.count(key) > 0) {
		return;
	}

	// A stream nobody is receiving has nothing to show in a window of its own, and popping it out would
	// leave a black rectangle on the desktop with no way to tell why.
	if (!surfaceIt->second.watching) {
		setWatching(senderSession, streamID, true);
	}

	// Shared with the window's callbacks rather than captured by value - see PipWindow. The stream id
	// inside it changes if the sender changes resolution or turns their phone, and the window follows.
	auto keyHolder = std::make_shared< std::uint64_t >(key);

	auto *window = new FullscreenVideoWindow(
		this,
		// Bound to this one surface rather than to whatever is focused, which is what lets several of
		// these exist at once showing different streams.
		[this, keyHolder](QImage &image, QString &label) {
			const auto it = m_surfaces.find(*keyHolder);

			if (it != m_surfaces.end()) {
				image = displayImage(it->second);
				label = labelForSurface(it->second);
			}
		},
		[this, keyHolder]() {
			FullscreenVideoWindow::VolumeState state;
			const auto it = m_surfaces.find(*keyHolder);

			if (it == m_surfaces.end() || it->second.sourceKind == MumbleProto::VideoState_SourceKind_Camera) {
				return state;
			}

			const auto level = m_shareVolumePercent.find(it->second.senderSession);
			state.available  = true;
			state.percent    = level != m_shareVolumePercent.end()
								   ? level->second
								   : static_cast< int >(AudioOutputScreenShare::DEFAULT_VOLUME * 100.0f);
			return state;
		},
		[this, keyHolder](int percent) {
			const auto it = m_surfaces.find(*keyHolder);

			if (it != m_surfaces.end()) {
				setShareVolumePercent(it->second.senderSession, percent);
			}
		},
		true, [this, keyHolder]() { popInSurfaceByKey(*keyHolder); });

	m_pipWindows[key]           = PipWindow{ window, keyHolder };
	surfaceIt->second.poppedOut = true;

	window->contentReplaced();
	window->setPresentation(true);

	// The tile has left the grid, so everything laid out around it moves up.
	emit senderCountChanged(tileCount());
	relayout();
}

void VideoGrid::destroyPipWindow(std::uint64_t key) {
	const auto windowIt = m_pipWindows.find(key);

	if (windowIt == m_pipWindows.end()) {
		return;
	}

	// deleteLater() rather than delete: this is reached from the window's own close handler, by way of its
	// closeEvent, so the window is still inside its own event delivery.
	windowIt->second.window->hide();
	windowIt->second.window->deleteLater();
	m_pipWindows.erase(windowIt);

	const auto surfaceIt = m_surfaces.find(key);

	if (surfaceIt != m_surfaces.end()) {
		surfaceIt->second.poppedOut = false;
	}
}

void VideoGrid::popInSurface(unsigned int senderSession, unsigned int streamID) {
	popInSurfaceByKey(surfaceKey(senderSession, streamID));
}

void VideoGrid::popInSurfaceByKey(std::uint64_t key) {
	if (!m_pipWindows.count(key)) {
		return;
	}

	destroyPipWindow(key);

	emit senderCountChanged(tileCount());
	relayout();
}

QImage VideoGrid::displayedImageForTesting(unsigned int senderSession, unsigned int streamID) const {
	const auto it = m_surfaces.find(surfaceKey(senderSession, streamID));

	return it == m_surfaces.end() ? QImage() : displayImage(it->second);
}

void VideoGrid::repointPresentations(std::uint64_t oldKey, std::uint64_t newKey) {
	if (oldKey == newKey) {
		return;
	}

	const auto windowIt = m_pipWindows.find(oldKey);

	if (windowIt != m_pipWindows.end()) {
		const PipWindow carried = windowIt->second;

		m_pipWindows.erase(windowIt);

		// Cannot happen - the new key belongs to a surface created moments ago, which has never been
		// popped out - but a window dropped from this map is a parentless top-level window nothing will
		// ever close, so it is worth not assuming.
		destroyPipWindow(newKey);

		// This is the part that makes the window show the new stream: its content callbacks read the
		// key through this pointer on every repaint, so they now resolve the replacement surface
		// instead of looking up one that no longer exists.
		*carried.key         = newKey;
		m_pipWindows[newKey] = carried;

		const auto surfaceIt = m_surfaces.find(newKey);

		if (surfaceIt != m_surfaces.end()) {
			// The replacement has moved out of the grid along with the window showing it.
			surfaceIt->second.poppedOut = true;
		}

		// Nothing of the old picture is still valid - a turned phone is a different shape - and there
		// is no patch of the new one yet, so the window is told to repaint whole rather than handed a
		// rectangle.
		static_cast< FullscreenVideoWindow * >(carried.window)->contentReplaced();
	}

	if (m_focus == FocusTarget::Surface && m_focusedSurfaceKey == oldKey) {
		m_focusedSurfaceKey = newKey;

		if (m_fullscreenWindow) {
			static_cast< FullscreenVideoWindow * >(m_fullscreenWindow)->contentReplaced();
		}
	}
}

void VideoGrid::setRemoteVideoHidden(bool hidden) {
	if (m_remoteVideoHidden == hidden) {
		return;
	}

	m_remoteVideoHidden = hidden;

	if (hidden) {
		// Remembered before unwatching, since unwatching is what clears the flag being read.
		m_watchedBeforeHidden.clear();

		for (const auto &entry : m_surfaces) {
			if (entry.second.watching) {
				m_watchedBeforeHidden.push_back(entry.first);
			}
		}

		// Unwatched, not merely hidden: a stream nobody can see should not still be arriving, and
		// unwatching is also what withdraws the subscription for any share audio that follows it.
		setWatchingAll(false);

		for (auto &entry : m_surfaces) {
			entry.second.hiddenByPreference = true;

			// A picture-in-picture window is a window on someone else's video, which is the thing being
			// hidden. It goes with the rest.
			destroyPipWindow(entry.first);
		}

		if (m_focus == FocusTarget::Surface) {
			m_focus = FocusTarget::None;
		}
	} else {
		for (auto &entry : m_surfaces) {
			entry.second.hiddenByPreference = false;
		}

		// Back to what was being watched, where that stream is still there. Someone whose share ended
		// while this was on is simply gone, which is the truth of it.
		for (const std::uint64_t key : m_watchedBeforeHidden) {
			const auto it = m_surfaces.find(key);

			if (it != m_surfaces.end()) {
				setWatching(it->second.senderSession, it->second.streamID, true);
			}
		}

		m_watchedBeforeHidden.clear();
	}

	emit senderCountChanged(tileCount());
	relayout();
}

void VideoGrid::setOwnViewers(bool screen, const std::vector< Viewer > &viewers) {
	(screen ? m_ownScreenViewers : m_ownCameraViewers) = viewers;

	if (TileControlBar *controls = screen ? m_ownScreenControls.get() : m_ownCameraControls.get()) {
		refreshViewersButton(controls, viewers);
	}

	if (m_fullscreenWindow && m_focus == (screen ? FocusTarget::SelfScreen : FocusTarget::SelfCamera)) {
		static_cast< FullscreenVideoWindow * >(m_fullscreenWindow)->contentReplaced();
	}

	update();
}

void VideoGrid::refreshViewersButton(TileControlBar *controls, const std::vector< Viewer > &viewers) {
	if (!controls || !controls->viewersButton) {
		return;
	}

	QStringList lines;
	QMenu *menu = controls->viewersButton->menu();
	menu->clear();

	for (const Viewer &viewer : viewers) {
		const QString line = viewer.volumePercent >= 0
								 ? tr("%1 - hears you at %2%").arg(viewer.name).arg(viewer.volumePercent)
								 : viewer.name;
		lines << line;
		// Enabled, though they do nothing: a disabled entry is greyed out and hard to read.
		menu->addAction(line);
	}

	if (viewers.empty()) {
		menu->addAction(tr("Nobody is watching"))->setEnabled(false);
	}

	controls->viewersButton->setText(QStringLiteral("👁 %1").arg(viewers.size()));
	controls->viewersButton->setToolTip(viewers.empty() ? tr("Nobody is watching")
														: tr("Watching:\n%1").arg(lines.join(QLatin1Char('\n'))));
}

QString VideoGrid::ownWatcherBadge(bool screen) const {
	return QStringLiteral("👁 %1").arg((screen ? m_ownScreenViewers : m_ownCameraViewers).size());
}

QString VideoGrid::ownTileLabel(bool screen) const {
	const QString base           = screen ? tr("You (screen)") : tr("You");
	const std::size_t watching = (screen ? m_ownScreenViewers : m_ownCameraViewers).size();

	return watching == 0 ? base : tr("%1 - %n watching", nullptr, static_cast< int >(watching)).arg(base);
}

int VideoGrid::shareVolumePercent(unsigned int senderSession) const {
	const auto it = m_shareVolumePercent.find(senderSession);

	return it != m_shareVolumePercent.end() ? it->second
											: static_cast< int >(AudioOutputScreenShare::DEFAULT_VOLUME * 100.0f);
}

void VideoGrid::setShareVolumePercent(unsigned int senderSession, int percent) {
	// Through the tile's own slider when there is one, so that it, its label and the audio all follow from
	// the one place a level is set.
	for (auto &entry : m_remoteControls) {
		const auto surface = m_surfaces.find(entry.first);
		if (surface != m_surfaces.end() && surface->second.senderSession == senderSession
			&& entry.second->volumeSlider) {
			entry.second->volumeSlider->setValue(percent);
			return;
		}
	}

	m_shareVolumePercent[senderSession] = percent;
	emit volumeChanged(senderSession, static_cast< float >(percent) / 100.0f);
}

void VideoGrid::notifyPipContentChanged(std::uint64_t key, const QRect &changed) {
	const auto it = m_pipWindows.find(key);

	if (it == m_pipWindows.end()) {
		return;
	}

	static_cast< FullscreenVideoWindow * >(it->second.window)->contentChanged(changed);
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
		if (!occupiesCell(it->second)) {
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

		// A surface still waiting for its replacement's first frame keeps the focus: it is blank for a
		// moment, not gone, and dropping focus here is what sent a viewer watching a rotating phone
		// fullscreen back to the grid.
		if (it == m_surfaces.end() || !it->second.watching
			|| (it->second.canvas.isNull() && !it->second.awaitingReplacementPicture)) {
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
		const QRect target = pictureRect(cell, image.size());

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

	// How many are watching an own tile, top right of its picture, always - the same count as the eye in
	// its control bar, which only shows while the mouse is over the tile. Whether anyone is watching is
	// the first thing a sharer wants to know, and having to hover to find out read as the count being
	// missing. Shown at zero too: "nobody yet" is an answer as well.
	const auto drawWatcherBadge = [&](const QImage &image, const QRect &cell, const QString &badge) {
		const QRect target = pictureRect(cell, image.size()).adjusted(4, 4, -4, -4);

		QRect backdrop = painter.fontMetrics().boundingRect(target, Qt::AlignTop | Qt::AlignRight, badge);
		backdrop.adjust(-4, -1, 4, 1);

		painter.fillRect(backdrop, QColor(0, 0, 0, 160));

		painter.setPen(Qt::white);
		painter.drawText(target, Qt::AlignTop | Qt::AlignRight, badge);
	};

	int index = 0;

	if (!m_selfCameraFrame.isNull()) {
		const QRect cell = cellRect(layout, index++);
		drawInto(m_selfCameraFrame, cell, tr("You"));
		drawWatcherBadge(m_selfCameraFrame, cell, ownWatcherBadge(false));
	}

	if (!m_selfScreenFrame.isNull()) {
		const QRect cell = cellRect(layout, index++);
		drawInto(m_selfScreenFrame, cell, tr("You (screen)"));
		drawWatcherBadge(m_selfScreenFrame, cell, ownWatcherBadge(true));
	}

	for (auto it = m_surfaces.cbegin(); it != m_surfaces.cend(); ++it) {
		const Surface &surface = it->second;

		if (!occupiesCell(surface)) {
			continue;
		}

		const QRect cell = cellRect(layout, index++);

		if (surface.watching) {
			drawInto(displayImage(surface), cell, labelForSurface(surface));
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
		if (!occupiesCell(it->second)) {
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
		if (!occupiesCell(it->second)) {
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
