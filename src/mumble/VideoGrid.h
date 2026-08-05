// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_VIDEOGRID_H_
#define MUMBLE_MUMBLE_VIDEOGRID_H_

#include "VP8Codec.h"

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtGui/QImage>
#include <QtWidgets/QWidget>

#include <cstdint>
#include <memory>
#include <unordered_map>

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

	explicit VideoGrid(QWidget *parent = nullptr);

	/// Number of senders currently on screen. Counts only those with a picture: a surface exists from a
	/// stream's announcement onward, but one with nothing decoded into it yet occupies no cell and must
	/// not reserve one, or the grid lays out around an empty square.
	int senderCount() const;

	/// Everything drawn, including your own picture. This is what decides whether the panel is worth
	/// showing at all.
	int tileCount() const { return senderCount() + (m_selfFrame.isNull() ? 0 : 1); }

	/// The surface accumulated for a sender, or a null image if there is none. Exposed so that the
	/// composition can be tested without going through a paint event.
	QImage surfaceFor(unsigned int senderSession) const;

public slots:
	/**
	 * Paints one decoded tile into its sender's surface.
	 *
	 * @param encodedTile A complete JPEG, as produced by TiledImageEncoder.
	 */
	void onVideoUnitReceived(unsigned int senderSession, unsigned int streamID, unsigned int x, unsigned int y,
							 const QByteArray &encodedTile);

	/// Shows the local camera. Drawn first so your own picture does not move about as other people come
	/// and go.
	void setSelfFrame(const QImage &frame);

	/// Stops showing the local camera.
	void clearSelfFrame();

	/**
	 * Records the codec a sender's stream is encoded with, from its VideoState announcement.
	 *
	 * Must arrive before the stream's units do, which the protocol guarantees: the announcement is what
	 * causes a receiver to subscribe, and the server relays nothing until it has.
	 *
	 * @param codec A MumbleProto::VideoState::Codec value.
	 */
	void setStreamCodec(unsigned int senderSession, unsigned int streamID, int codec);

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

	/// Drops a sender's surface, on disconnect or when their stream ends.
	void removeSender(unsigned int senderSession);

	/// Drops everything, on disconnect from the server.
	void clear();

signals:
	/// Emitted when a sender appears or disappears, so the containing window can show or hide itself.
	void senderCountChanged(int count);

protected:
	struct Surface {
		QImage canvas;
		unsigned int streamID = 0;

		/// MumbleProto::VideoState::Codec. 0 is CODEC_UNKNOWN, whose units are dropped.
		int codec = 0;

		/// Drawn on the tile. Empty until the sender is identified.
		QString name;

		/// Created only for streams that need it, and destroyed with the stream: a VP8 decoder carries
		/// reference frames, so reusing one across streams would decode new frames against stale state.
		std::unique_ptr< VP8Decoder > vp8;
	};

	// std::unordered_map rather than QHash: a Surface owns its decoder through a unique_ptr, which makes
	// it move-only, and QHash requires its values to be copyable.
	std::unordered_map< unsigned int, Surface > m_surfaces;

	/// The local camera, kept apart from m_surfaces because it has no session and is always drawn first.
	QImage m_selfFrame;

	void paintEvent(QPaintEvent *event) override;

	/// Grows a surface so the given rectangle fits, within the bounds above. Returns false if the
	/// rectangle cannot be accommodated.
	static bool growToFit(QImage &canvas, int x, int y, int width, int height);

	/// Turns one unit's payload into an image using the codec the stream announced. Returns a null
	/// image if the codec is unknown, unsupported, or the payload did not decode.
	static QImage decodeUnit(Surface &surface, const QByteArray &payload);
};

#endif // MUMBLE_MUMBLE_VIDEOGRID_H_
