// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_VIDEOGRID_H_
#define MUMBLE_MUMBLE_VIDEOGRID_H_

#include <QtCore/QByteArray>
#include <QtCore/QHash>
#include <QtGui/QImage>
#include <QtWidgets/QWidget>

#include <cstdint>

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

	/// Number of senders currently on screen.
	int senderCount() const { return static_cast< int >(m_surfaces.size()); }

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
	};

	QHash< unsigned int, Surface > m_surfaces;

	/// The local camera, kept apart from m_surfaces because it has no session and is always drawn first.
	QImage m_selfFrame;

	void paintEvent(QPaintEvent *event) override;

	/// Grows a surface so the given rectangle fits, within the bounds above. Returns false if the
	/// rectangle cannot be accommodated.
	static bool growToFit(QImage &canvas, int x, int y, int width, int height);
};

#endif // MUMBLE_MUMBLE_VIDEOGRID_H_
