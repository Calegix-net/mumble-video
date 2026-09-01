// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_VIDEOGRID_H_
#define MUMBLE_MUMBLE_VIDEOGRID_H_

#include "VP8Codec.h"

#include <QtCore/QByteArray>
#include <QtCore/QPoint>
#include <QtCore/QString>
#include <QtGui/QImage>
#include <QtWidgets/QWidget>

#include <cstdint>
#include <map>
#include <memory>
#include <unordered_map>

class QEnterEvent;
class QKeyEvent;
class QMouseEvent;
class QResizeEvent;
class QSlider;
class QTimer;
class QToolButton;

/**
 * Displays the video other participants are sharing.
 *
 * Each sender gets one surface that tiles are painted into as they arrive. That is the whole reason the
 * TiledImage codec works: a tile is a complete JPEG covering a known rectangle, so a surface can be
 * updated in pieces and remains coherent even when some tiles are lost, rather than needing a whole
 * frame before anything can be shown.
 *
 * Surfaces are grown to fit the tiles that arrive rather than sized from an announced resolution. The
 * announcement travels over TCP and the tiles over UDP, so the two can arrive in either order, and a
 * surface that has to wait for the announcement would show nothing until it turned up.
 *
 * Everything reaching this class has been relayed by the server and therefore originates with another
 * client, so the geometry on a tile is treated as untrusted: surfaces are bounded, tiles that would land
 * outside one are refused, and the number of senders on screen is capped.
 *
 * Which decoder a unit is fed to comes from the stream's announcement, never from the bytes themselves.
 * The payload is attacker-controlled, so letting it select the decoder would turn every format the
 * client can parse into attack surface. A stream whose codec was never announced, or was announced as
 * one this build does not know, is dropped rather than guessed at.
 *
 * Your own camera and your own screen share each get a permanently reserved cell of their own - see
 * m_selfCameraFrame/m_selfScreenFrame - so that another participant's stream starting, stopping or being
 * re-announced can never so much as move your own tile, let alone hide it. Every other tile carries a
 * small hover-reveal control strip: a fullscreen toggle on every tile, and, on a remote sender's tile, an
 * eyeball watch/unwatch toggle and - for a screen share specifically, which is the only kind of stream
 * with audio of its own - a volume slider.
 *
 * A remote stream starts out unwatched: announced, it claims its cell and shows a greyed-out preview
 * placeholder immediately, but nothing is actually decoded - and nothing is even asked of the server -
 * until its eyeball is clicked (or its placeholder double-clicked, the same thing by the more discoverable
 * route every other tile's double-click already means). This is what keeps many people sharing at once
 * affordable: the cost of decoding a stream is only ever paid for the ones somebody actually chose to
 * watch. Watching a stream and then choosing to stop unsubscribes from it over the network (the sender
 * keeps broadcasting to everyone else) and returns its cell to the same placeholder, rather than removing
 * the tile and its cell outright, which would just be this class's own reshuffling bug by another name.
 */
class VideoGrid : public QWidget {
	Q_OBJECT

public:
	/// Widest and tallest a surface may become. A tile claiming to belong at a huge offset must not be
	/// able to make the client allocate a correspondingly huge image.
	static constexpr int MAX_SURFACE_WIDTH  = 3840;
	static constexpr int MAX_SURFACE_HEIGHT = 2160;

	/// How many senders are drawn at once. Past this the cells are too small to be worth anything, and
	/// each surface costs real memory.
	static constexpr int MAX_SENDERS = 16;

	/// Consecutive undecodable units from one stream before keyframeNeeded() is emitted. VP8 after lost
	/// reference frames fails on every unit until a keyframe arrives, so a run of this length means
	/// waiting will not help. High enough that a single corrupt unit stays a non-event.
	static constexpr int KEYFRAME_REQUEST_AFTER_FAILURES = 10;

	/// While frozen waiting for a keyframe, re-ask after this many dropped inter-frames (about a second
	/// at 30 fps) in case the first request was lost. The server rate-limits relays regardless.
	static constexpr int KEYFRAME_REREQUEST_AFTER_UNITS = 30;

	/// Default for m_staleStreamTimeoutMsec (see there) - generous deliberately: TiledImageEncoder's own
	/// periodic per-tile refresh (see VideoEncoder.h) means a genuinely alive, entirely static screen
	/// share still produces some traffic well inside this window at any ordinary framerate, so the real
	/// risk being guarded against is a false drop of a legitimately quiet stream, not a slow reaction to a
	/// dead one.
	static constexpr int DEFAULT_STALE_STREAM_TIMEOUT_MSEC = 45000;

	/// Default for m_stallRefreshTimeoutMsec (see there). Comfortably longer than TiledImageEncoder's own
	/// 2-second periodic refresh cycle (VideoEncoder.h's FULL_REFRESH_INTERVAL_FRAMES at an ordinary
	/// framerate), so ordinary jitter in when that cycle's next tile happens to land does not itself look
	/// like a stall - but far short of DEFAULT_STALE_STREAM_TIMEOUT_MSEC, which exists to catch a stream
	/// that is not coming back at all, not one that is merely overdue for its next update.
	static constexpr int DEFAULT_STALL_REFRESH_TIMEOUT_MSEC = 4000;

	explicit VideoGrid(QWidget *parent = nullptr);

	/// Shrinks the stale-stream watchdog's timeout (see checkForStaleStreams()) and restarts its poll
	/// timer to match - exposed purely so a test can exercise the real timer-driven path with
	/// QTest::qWait() in a reasonable amount of wall-clock time, rather than either waiting out the real
	/// 45-second default or calling checkForStaleStreams() directly and never actually exercising the
	/// timer wiring it normally runs from. Not meant to be called outside a test.
	void setStaleStreamTimeoutMsecForTesting(int msec);

	/// The stall-refresh sibling of setStaleStreamTimeoutMsecForTesting() above - same reasoning, same
	/// caveat: not meant to be called outside a test. Also restarts the poll timer: it is driven by
	/// whichever of the two timeouts is currently smaller (see applyPollInterval()), and the whole point
	/// of this setter is usually to make the stall threshold the tighter of the two.
	void setStallRefreshTimeoutMsecForTesting(int msec);

	/// How many times relayoutControls() has actually run - real per-tile widget work (setGeometry(),
	/// raise(), sometimes creating a QWidget), unlike the update() a routine tile content change now uses
	/// instead. Exposed purely so a test can prove that a tile updating within an already-laid-out surface
	/// does not pay this cost, rather than trusting a comment that it does not. Not meant to be called
	/// outside a test.
	int relayoutControlsCallCountForTesting() const { return m_relayoutControlsCallCount; }

	/// Number of other participants' tiles currently drawable - one per stream, not one per person, since
	/// a sender may hold more than one stream (a camera and a screen, say). A stream not being watched
	/// occupies its cell the moment it is announced, showing a preview placeholder there - it does not
	/// wait on a canvas that, unwatched, will never fill. A stream still being watched but genuinely blank
	/// - announced, but its first tile has not arrived yet - occupies no cell and must not reserve one, or
	/// the grid lays out around an empty square.
	int senderCount() const;

	/// Everything drawn, including your own camera and your own screen share. This is what decides
	/// whether the panel is worth showing at all.
	int tileCount() const {
		return senderCount() + (m_selfCameraFrame.isNull() ? 0 : 1) + (m_selfScreenFrame.isNull() ? 0 : 1);
	}

	/// The surface accumulated for one of a sender's streams, or a null image if there is none. Exposed
	/// so that the composition can be tested without going through a paint event.
	QImage surfaceFor(unsigned int senderSession, unsigned int streamID) const;

	/// Whether some tile is currently expanded to fill the whole grid - the "click to fullscreen a
	/// stream" behaviour every other video call offers. Exposed so the containing window can, for
	/// instance, know to hide unrelated chrome while a tile fills it.
	bool hasFocusedTile() const { return m_focus != FocusTarget::None; }

public slots:
	/**
	 * Paints one decoded tile into its sender's surface.
	 *
	 * @param encodedTile A complete JPEG, as produced by TiledImageEncoder.
	 */
	void onVideoUnitReceived(unsigned int senderSession, unsigned int streamID, quint64 frameNumber, bool isKeyframe,
							 unsigned int x, unsigned int y, const QByteArray &encodedTile);

	/// Shows your own camera in its own permanently reserved cell.
	void setSelfCameraFrame(const QImage &frame);

	/// Stops showing your own camera.
	void clearSelfCameraFrame();

	/// Shows your own screen share in its own permanently reserved cell - a separate one from your
	/// camera's, so sharing both at once shows both, rather than the two fighting over one slot.
	void setSelfScreenFrame(const QImage &frame);

	/// Stops showing your own screen share.
	void clearSelfScreenFrame();

	/**
	 * Records the codec and source kind a sender's stream carries, from its VideoState announcement.
	 *
	 * A stream this grid has not seen before claims a cell right away, as an unwatched preview - see
	 * Surface::watching - rather than being subscribed to and decoded immediately.
	 *
	 * @param sourceKind A MumbleProto::VideoState::SourceKind value, used only to label the tile.
	 * @param codec A MumbleProto::VideoState::Codec value.
	 */
	void setStreamCodec(unsigned int senderSession, unsigned int streamID, int sourceKind, int codec);

	/**
	 * Marks one of a sender's streams as watched or not - exactly what clicking its tile's own eyeball
	 * button, or double-clicking its preview placeholder, does. Exposed as its own slot rather than left
	 * as private lambda logic on those two controls, both so the two do not each carry their own copy of
	 * it and so it can be driven directly - by a test, or in principle by anything else that learns a
	 * subscription's state changed by some route other than a click.
	 *
	 * Does nothing if the sender holds no such stream, or is already in the requested state.
	 */
	void setWatching(unsigned int senderSession, unsigned int streamID, bool watching);

	/**
	 * Sets the name drawn on a sender's tile.
	 *
	 * Passed in rather than looked up, so the grid stays a plain widget over session ids and can be
	 * tested without the client's user model. An unnamed sender is drawn with no label rather than with
	 * a session number, which would mean nothing to anybody looking at it.
	 */
	void setSenderName(unsigned int senderSession, const QString &name);

	/// The name a sender's tile is labelled with, empty if none is known.
	QString senderName(unsigned int senderSession) const;

	/// Drops one of a sender's streams, when that stream ends. Their other streams, if any, are
	/// unaffected - ending a screen share must not blank a still-active camera tile from the same person.
	void removeSender(unsigned int senderSession, unsigned int streamID);

	/// Drops everything belonging to a sender, on disconnect.
	void removeSender(unsigned int senderSession);

	/// Drops everything, on disconnect from the server.
	void clear();

	/// Returns to the normal grid layout, if some tile is currently expanded to fill it. Exposed as its
	/// own slot, distinct from double-clicking, so a window-level "Esc" shortcut or an on-screen button
	/// can leave fullscreen without simulating a click.
	void clearFocusedTile();

signals:
	/// Emitted when a sender appears or disappears, so the containing window can show or hide itself.
	void senderCountChanged(int count);

	/// Emitted when a stream has produced KEYFRAME_REQUEST_AFTER_FAILURES undecodable units in a row,
	/// meaning its decoder has lost the reference frames it needs and only a keyframe can restart it.
	/// The grid has no connection of its own, so acting on this is the owner's job.
	void keyframeNeeded(unsigned int senderSession, unsigned int streamID);

	/// The watch/unwatch button on a remote sender's tile was clicked. The grid has already updated its
	/// own local rendering (blanking the tile to a placeholder, or restoring it); this is what the owner
	/// uses to actually send the VideoSubscribe that makes the server start or stop relaying the stream -
	/// unwatching is a subscription a listener can drop, not something that touches the sender at all.
	void watchToggled(unsigned int senderSession, unsigned int streamID, bool wantToWatch);

	/// The volume slider on a remote sender's screen-share tile moved. 0.0 is silent, 1.0 is unity gain -
	/// the grid has no audio of its own to apply this to, so the owner is what forwards it to whichever
	/// buffer is actually playing that sender's screen-share audio.
	void volumeChanged(unsigned int senderSession, float multiplier);

	/// A watched stream stopped producing units for long enough to be treated as dead and dropped - see
	/// checkForStaleStreams(). The grid has already removed its own surface by the time this fires; this
	/// is what lets the owner also withdraw the now-pointless subscription, the same VideoSubscribe(false)
	/// a normal unwatch sends, so the server stops relaying a stream nothing is looking at any more.
	void streamWentStale(unsigned int senderSession, unsigned int streamID);

protected:
	struct Surface {
		QImage canvas;
		unsigned int senderSession = 0;
		unsigned int streamID      = 0;

		/// MumbleProto::VideoState::Codec. 0 is CODEC_UNKNOWN, whose units are dropped.
		int codec = 0;

		/// MumbleProto::VideoState::SourceKind. 0 is SOURCE_UNKNOWN. Used only to label the tile - a
		/// sender showing a camera and a screen at once needs the two told apart, since they now share
		/// the same name.
		int sourceKind = 0;

		/// Undecodable units in a row, for the keyframeNeeded() threshold. Reset by any success.
		int consecutiveFailures = 0;

		/// Highest frame number successfully decoded, once anything has been. VP8 only: inter-frames
		/// reference their predecessor, so a number that skips ahead means the reference was lost and
		/// the next decode would *succeed* with garbage output - the green-frame artifact - rather
		/// than fail. Gaps therefore freeze the picture until the next keyframe instead of decoding.
		quint64 lastFrameNumber = 0;
		bool hasDecodedFrame    = false;

		/// Frozen: a gap was seen and only a keyframe may resume decoding. Suppresses repeated
		/// keyframeNeeded spam; dropped units are counted so the request can be repeated if lost.
		bool awaitingKeyframe   = false;
		int unitsWhileAwaiting  = 0;

		/// False for a brand new stream - it starts as a greyed-out preview, and only actually decodes
		/// once its tile's eyeball is clicked - and left alone by whatever an existing surface's stream id
		/// changing under it resets, so a resolution or codec change mid-share does not silently revert a
		/// choice the user already made either way. A unit arriving for a stream that is not being watched
		/// is dropped unread - see onVideoUnitReceived() - both because there is nothing to paint it into
		/// right now and because the server is expected to stop sending them shortly after
		/// watchToggled(false) goes out; this is the defensive side of that, not the mechanism itself.
		bool watching = false;

		/// QDateTime::currentMSecsSinceEpoch() the last time a unit arrived for this stream - including
		/// one dropped because the surface was not being watched at the time, and reset again the instant
		/// watching starts, so a stream that sat unwatched for a long time does not look stale the moment
		/// someone finally clicks its eyeball. Read by the stale-stream watchdog in relayout()'s timer -
		/// see checkForStaleStreams() - which exists because a sender whose disconnect the server never
		/// gets to relay (its own crash left the connection in a state nothing here notices) otherwise
		/// leaves its last frame on screen forever: nothing about VideoState(active=false) ever arrives to
		/// remove it, and there is no other signal that the stream is actually gone.
		qint64 lastUnitMsec = 0;

		/// Set once a stall refresh request has gone out for the current silence - see
		/// checkForStaleStreams() - so the same stall does not re-trigger keyframeNeeded() on every poll
		/// between the stall threshold and the much longer stale-drop one. Cleared alongside lastUnitMsec
		/// whenever a real unit actually arrives, so the next stall gets its own fresh request.
		bool stallRefreshRequested = false;

		/// Created only for streams that need it, and destroyed with the stream: a VP8 decoder carries
		/// reference frames, so reusing one across streams would decode new frames against stale state.
		std::unique_ptr< VP8Decoder > vp8;
	};

	/// Combines a sender and their stream into one lookup key. A sender may hold several streams open at
	/// once (camera, screen, screen audio), and nothing about the surfaces themselves may be conflated
	/// across them - least of all a VP8 decoder's reference frames.
	static std::uint64_t surfaceKey(unsigned int senderSession, unsigned int streamID) {
		return (static_cast< std::uint64_t >(senderSession) << 32) | streamID;
	}

	/// The layout paintEvent() and hit-testing both need: how many drawable tiles there are right now and
	/// which columns/rows they form a roughly-square arrangement out of. Shared so a click can never land
	/// on a different cell than the same point was last painted into. Declared up here, ahead of its
	/// first use below, rather than down with the rest of the layout helpers - a member function
	/// declaration (unlike one defined inline) needs a nested type to already be visible at that point in
	/// the class body, not merely somewhere else in it.
	struct Layout {
		int count      = 0;
		int columns    = 0;
		int rows       = 0;
		int cellWidth  = 0;
		int cellHeight = 0;
	};

	Layout currentLayout() const;

	/// Which slot index (see Layout) a point falls in, or -1 if it is outside every cell - the margins
	/// around a letterboxed picture, or simply past the last occupied cell in an incomplete row.
	int slotAt(const QPoint &point, const Layout &layout) const;

	/// The pixel rect of one slot in the given layout - shared by paintEvent()'s drawing and
	/// relayoutControls()'s widget placement so a tile's picture and its own control bar can never
	/// disagree about where the tile actually is.
	static QRect cellRect(const Layout &layout, int slot);

	// std::map, not QHash or std::unordered_map. QHash requires its values to be copyable, and a Surface
	// owns its decoder through a unique_ptr, which makes it move-only. std::map over std::unordered_map
	// specifically: paintEvent() below assigns each surface's grid cell by iteration order, and unordered
	// iteration order is not stable across inserts/erases - toggling any one stream, including your own,
	// could silently reshuffle every other sender's cell in the same repaint. A sorted map's order depends
	// only on the keys present, never on insertion/erasure history, so a sender's cell only moves when the
	// set of visible senders actually changes - and the map is capped at MAX_SENDERS entries, so the
	// O(log n) it costs over a hash map is not worth worrying about.
	std::map< std::uint64_t, Surface > m_surfaces;

	/// Number of distinct senders currently holding at least one surface, regardless of how many streams
	/// each holds. What MAX_SENDERS actually bounds: the cap exists to stop the grid drawing more people
	/// than a cell size can make worthwhile, not to stop one person from sharing a camera and a screen.
	std::size_t distinctSenderCount() const;

	/// Drawn on a sender's tile. Kept apart from Surface because a name belongs to the person, not to any
	/// one of their streams.
	std::unordered_map< unsigned int, QString > m_senderNames;

	/// Your own camera and your own screen share, kept apart both from m_surfaces (they have no session)
	/// and from each other (see the class comment for why they used to share one slot, and why that was
	/// a bug worth fixing rather than a limitation worth documenting).
	QImage m_selfCameraFrame;
	QImage m_selfScreenFrame;

	/// What, if anything, is expanded to fill the whole grid instead of taking its usual cell.
	enum class FocusTarget { None, SelfCamera, SelfScreen, Surface };

	FocusTarget m_focus = FocusTarget::None;

	/// Meaningful only when m_focus == FocusTarget::Surface. Not cleared when focus moves away from
	/// Surface, so nothing has to reset it defensively - the enum tag is the source of truth for whether
	/// it means anything right now.
	std::uint64_t m_focusedSurfaceKey = 0;

	/// The always-on control strip drawn across the bottom of a tile. One real QWidget (not hand-painted)
	/// so its buttons and slider get ordinary click/drag handling for free; positioned by relayoutControls()
	/// to sit at the bottom of whatever cell rect the tile currently occupies, which changes every time the
	/// layout does. fullscreenButton exists on every tile, own or remote; watchButton and volumeSlider are
	/// only ever non-null on a remote sender's tile, and volumeSlider only on one carrying a screen share.
	struct TileControlBar {
		QWidget *bar               = nullptr;
		QToolButton *fullscreenButton = nullptr;
		QToolButton *watchButton      = nullptr;
		QSlider *volumeSlider          = nullptr;

		/// bar owns fullscreenButton/watchButton/volumeSlider through Qt's own parent-child ownership - they
		/// are all constructed with bar as their parent - but bar itself is a plain QWidget*, not something
		/// Qt destroys on its own just because this struct goes away. Without this, every place a
		/// TileControlBar is destroyed (relayoutControls() dropping a bar for a stream that ended,
		/// m_ownCameraControls/m_ownScreenControls.reset(), clear() on disconnect) orphaned the widget
		/// instead: still alive, still parented to VideoGrid, still fully wired to its signal/slot
		/// connections, just no longer tracked or positioned by anything - a real, unbounded leak across a
		/// session with any amount of stream churn. deleteLater() rather than a direct delete: this runs
		/// from inside relayoutControls(), reachable from a bar's own button's clicked() handler by way of
		/// setWatching(), and a widget must never be deleted out from under an event still being delivered
		/// to it.
		~TileControlBar() {
			if (bar) {
				bar->deleteLater();
			}
		}
	};

	/// Resets m_focus to None if whatever it currently points at no longer has anything to show - a
	/// stream that ended, or one just unwatched from its own control bar. Called from both relayout() and
	/// paintEvent(), so a bar's geometry and the picture actually painted can never disagree about
	/// whether something is still focused.
	void validateFocus();

	std::unique_ptr< TileControlBar > m_ownCameraControls;
	std::unique_ptr< TileControlBar > m_ownScreenControls;
	std::map< std::uint64_t, std::unique_ptr< TileControlBar > > m_remoteControls;

	/// Whether the cursor is currently over this widget at all, and where - see updateHoveredBar(). A
	/// plain QWidget*, not a smart pointer to a locally-defined type: FullscreenVideoWindow is declared in
	/// VideoGrid.cpp, not here, and it is parented to this widget, so Qt's own parent/child ownership is
	/// what actually destroys it - this pointer never has to.
	bool m_mouseInside = false;
	QPoint m_lastMousePos;
	QWidget *m_fullscreenWindow = nullptr;

	/// The slot index (see Layout) updateHoveredBar() last actually showed a bar for, or -1 if none.
	/// mouseMoveEvent() checks this before calling updateHoveredBar() at all: Qt delivers a move event for
	/// every pixel the cursor crosses while gliding across the grid, far more often than the hovered tile
	/// actually changes, and redoing every bar's show/hide on each one is a real, measurable amount of
	/// wasted widget churn for something that produces no visible difference most of the time it runs.
	int m_hoveredSlot = -1;

	/// Set for the duration of a relayout() call, and checked at its own start: several paths reach here
	/// synchronously from inside another relayout() already in progress - most notably, the video dock
	/// becoming visible for the first time can synchronously fire this widget's own resizeEvent() before
	/// the relayout() that triggered it has returned. relayoutControls() creates and destroys real
	/// QWidgets; letting two passes of it interleave is exactly the kind of Qt reentrancy that already
	/// caused two access violations earlier in this class's history. The in-progress pass already reflects
	/// whatever state change asked for the nested one, so skipping the nested call loses nothing.
	bool m_relayoutInProgress = false;

	/// Polls for streams that have gone quiet - see checkForStaleStreams(). A plain periodic timer rather
	/// than something scheduled per-stream: the check itself is cheap (a linear scan of a map capped at
	/// MAX_SENDERS entries), and a single shared timer needs no bookkeeping to add or remove as streams
	/// come and go.
	QTimer *m_staleCheckTimer = nullptr;

	/// See DEFAULT_STALE_STREAM_TIMEOUT_MSEC and setStaleStreamTimeoutMsecForTesting(). A runtime value
	/// rather than a compile-time constant purely for that testability - nothing in ordinary use ever
	/// changes it from the default.
	int m_staleStreamTimeoutMsec = DEFAULT_STALE_STREAM_TIMEOUT_MSEC;

	/// See DEFAULT_STALL_REFRESH_TIMEOUT_MSEC and setStallRefreshTimeoutMsecForTesting().
	int m_stallRefreshTimeoutMsec = DEFAULT_STALL_REFRESH_TIMEOUT_MSEC;

	/// (Re)starts m_staleCheckTimer at a tenth of whichever of m_staleStreamTimeoutMsec and
	/// m_stallRefreshTimeoutMsec is smaller. Both setters above call this rather than deriving the
	/// interval from the stale timeout alone: the stall threshold is the shorter of the two by design
	/// (see DEFAULT_STALL_REFRESH_TIMEOUT_MSEC's doc comment), and a poll interval derived only from the
	/// longer one can be coarser than the shorter threshold it is supposed to be catching - at the
	/// defaults, 4500ms polls against a 4000ms stall threshold, which lets a stall run up to ~8.5s before
	/// it is ever noticed instead of the intended ~4s.
	void applyPollInterval();

	/// See relayoutControlsCallCountForTesting(). Incremented at the top of relayoutControls() itself, so
	/// it counts every real call regardless of which of relayout()'s several call sites reached it.
	int m_relayoutControlsCallCount = 0;

	/// Drops a watched stream nothing has arrived for in m_staleStreamTimeoutMsec, exactly as if its
	/// sender's VideoState(active=false) had arrived - because for the case this exists to catch, no such
	/// message ever will. A sender's own client crashing does not, by itself, guarantee the server notices
	/// promptly: TCP teardown depends on the OS actually closing the socket, which a crash that leaves the
	/// process in a non-terminating state (still generating a crash dump, say, or stuck behind a debugger
	/// prompt nobody is there to dismiss) can leave undone indefinitely - and until the server notices, it
	/// has nothing to relay to anyone else, so no client-side fix downstream of that message can help
	/// either. This is the belt-and-suspenders alternative: judge staleness from silence on the stream
	/// itself, not from being told it ended.
	void checkForStaleStreams();

	/// Builds a bar with just a fullscreen button - what every own tile gets.
	std::unique_ptr< TileControlBar > makeOwnControlBar(FocusTarget target);

	/// Builds a bar with a fullscreen button, a watch/unwatch button, and - only when hasAudio - a volume
	/// slider, wired to emit watchToggled()/volumeChanged() for the given (senderSession, streamID).
	std::unique_ptr< TileControlBar > makeRemoteControlBar(unsigned int senderSession, unsigned int streamID,
														   bool hasAudio);

	/// Positions every control bar to match the tile currently occupying each cell - creating bars for
	/// tiles that just gained one, destroying them for tiles that are gone, and moving the rest to
	/// wherever the current layout puts their cell. Bars are positioned but left hidden here; whether one
	/// is actually visible is entirely updateHoveredBar()'s job, called right after this by relayout().
	///
	/// Deliberately never called from paintEvent(): creating or showing a QWidget can, in general, pump
	/// enough of the event queue to re-enter painting on its parent - and doing that while this widget's
	/// own QPainter is still alive is exactly the kind of reentrancy Qt's docs warn painting must never
	/// risk. relayout() below is the single call site every state change goes through instead, precisely
	/// so control-bar bookkeeping and repainting can never interleave.
	void relayoutControls(const Layout &layout);

	/// Shows the control bar for whichever tile the cursor is currently over, and hides every other one -
	/// bars are hover-reveal, not always-on, so a tile's picture is not permanently sitting under a
	/// translucent strip nobody is interacting with. Uses m_mouseInside/m_lastMousePos rather than an
	/// event's own position, since this also has to re-run whenever the layout changes under a stationary
	/// cursor - a tile appearing or disappearing elsewhere can move the cell the cursor is already over.
	void updateHoveredBar();

	/// Builds and (still hidden) positions the picture-in-picture window a fullscreened tile is actually
	/// shown in - see FullscreenVideoWindow's own comment for why the grid does not simply fill itself the
	/// way an early version did. Shows, updates, or hides that window to match m_focus, called from
	/// relayout() so it can never fall out of sync with which tile paintEvent() and the control bars agree
	/// is focused.
	void updateFullscreenWindow();

	/// The name and, for anything other than a camera, the kind of stream a tile is showing - "Screen",
	/// "Window", "App" - shared between paintEvent()'s labels and the fullscreen window's, so the two
	/// never describe the same tile differently.
	QString labelForSurface(const Surface &surface) const;

	/// Recomputes the layout, brings every control bar and the fullscreen window in line with it, and
	/// schedules a repaint - the one place "something changed, the grid may need to look different" goes
	/// through, called from every slot and click handler that used to call update() directly, and from
	/// resizeEvent() below for a plain window resize that does not otherwise change what update() would
	/// have repainted anyway.
	void relayout();

	void paintEvent(QPaintEvent *event) override;

	void resizeEvent(QResizeEvent *event) override;

	void mouseMoveEvent(QMouseEvent *event) override;
	void enterEvent(QEnterEvent *event) override;
	void leaveEvent(QEvent *event) override;

	/// Toggles fullscreen on whichever tile is under the cursor: focuses it if nothing is focused, returns
	/// to the grid if that same tile already is, or moves focus straight to the newly clicked one if a
	/// different tile was focused. Matches the "double-click a tile to expand it, double-click again to
	/// shrink it back" convention every other video call uses. The fullscreen button on each tile's control
	/// bar does the same thing by the more discoverable route of an actual click target.
	void mouseDoubleClickEvent(QMouseEvent *event) override;

	/// Esc leaves fullscreen, the same key every other view in this application already uses to back out
	/// of something. Does nothing when no tile is focused, rather than consuming the event, so a plain Esc
	/// still reaches whatever else might want it when the grid is not the reason it was pressed.
	void keyPressEvent(QKeyEvent *event) override;

public:
	QSize sizeHint() const override;

protected:

	/// Grows a surface so the given rectangle fits, within the bounds above. Returns false if the
	/// rectangle cannot be accommodated.
	static bool growToFit(QImage &canvas, int x, int y, int width, int height);

	/// Turns one unit's payload into an image using the codec the stream announced. Returns a null
	/// image if the codec is unknown, unsupported, or the payload did not decode.
	static QImage decodeUnit(Surface &surface, const QByteArray &payload);

	/// The TiledImage half of decodeUnit(), pulled out on its own: unlike VP8, whose decoder carries
	/// reference-frame state that must only ever be touched from one thread in strict order, a JPEG tile
	/// is a pure function of its own bytes alone - nothing else about the surface it belongs to matters to
	/// decoding it. That statelessness is what makes it safe to run this off the GUI thread at all, which
	/// onVideoUnitReceived() does for exactly this codec. decodeUnit() itself still calls this for the
	/// synchronous TestVideoGrid-facing cases that expect one.
	static QImage decodeJpegTile(const QByteArray &payload);

	/// The continuation of onVideoUnitReceived() for a TiledImage unit, once its JPEG decode - dispatched
	/// there to a background thread, see the comment at that call site - has actually finished. Re-looks
	/// up the surface by key rather than capturing a reference to it: the decode ran asynchronously, so
	/// the surface may have been removed, or stopped being watched, in the meantime, and a stale result
	/// must be discarded rather than painted into whatever now occupies - or no longer occupies - that
	/// slot in m_surfaces.
	void onTiledImageTileDecoded(unsigned int senderSession, unsigned int streamID, unsigned int x, unsigned int y,
								 QImage tile);

	/// Paints one successfully decoded tile into its surface's canvas and brings the rest of the grid in
	/// line with it - shared by onVideoUnitReceived()'s own synchronous VP8 path and
	/// onTiledImageTileDecoded()'s asynchronous one, so the two never have a reason to disagree about what
	/// a successful decode actually does.
	void applyDecodedTile(std::uint64_t key, Surface &surface, unsigned int x, unsigned int y, const QImage &tile);
};

#endif // MUMBLE_MUMBLE_VIDEOGRID_H_
