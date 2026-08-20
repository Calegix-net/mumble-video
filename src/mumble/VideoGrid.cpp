// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "VideoGrid.h"

#include "Mumble.pb.h"

#include <QtGui/QPainter>

#include <algorithm>
#include <cmath>
#include <utility>

VideoGrid::VideoGrid(QWidget *parent) : QWidget(parent) {
	setAutoFillBackground(true);
}

int VideoGrid::senderCount() const {
	int count = 0;

	for (auto it = m_surfaces.cbegin(); it != m_surfaces.cend(); ++it) {
		if (!it->second.canvas.isNull()) {
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

	update();
}

QString VideoGrid::senderName(unsigned int senderSession) const {
	const auto it = m_senderNames.find(senderSession);

	return it == m_senderNames.end() ? QString() : it->second;
}

void VideoGrid::setStreamCodec(unsigned int senderSession, unsigned int streamID, int sourceKind, int codec) {
	const std::uint64_t key = surfaceKey(senderSession, streamID);

	if (m_surfaces.find(key) == m_surfaces.end()) {
		bool senderAlreadyPresent = false;

		for (auto it = m_surfaces.cbegin(); it != m_surfaces.cend(); ++it) {
			if (it->second.senderSession == senderSession) {
				senderAlreadyPresent = true;
				break;
			}
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
	}

	surface.senderSession = senderSession;
	surface.streamID      = streamID;
	surface.codec         = codec;
	surface.sourceKind    = sourceKind;
}

void VideoGrid::onVideoUnitReceived(unsigned int senderSession, unsigned int streamID, unsigned int x, unsigned int y,
									const QByteArray &encodedTile) {
	// Only streams that announced themselves are decoded. Units arriving for an unannounced stream have
	// no codec, and are dropped rather than assumed to be anything.
	const auto existing = m_surfaces.find(surfaceKey(senderSession, streamID));

	if (existing == m_surfaces.end()) {
		return;
	}

	Surface &surface = existing->second;

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

	update();
}

void VideoGrid::setSelfFrame(const QImage &frame) {
	const bool wasEmpty = m_selfFrame.isNull();

	m_selfFrame = frame;

	if (wasEmpty != frame.isNull()) {
		emit senderCountChanged(tileCount());
	}

	update();
}

void VideoGrid::clearSelfFrame() {
	if (m_selfFrame.isNull()) {
		return;
	}

	m_selfFrame = QImage();

	emit senderCountChanged(tileCount());
	update();
}

void VideoGrid::removeSender(unsigned int senderSession, unsigned int streamID) {
	if (m_surfaces.erase(surfaceKey(senderSession, streamID)) > 0) {
		emit senderCountChanged(senderCount());
		update();
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
		update();
	}
}

void VideoGrid::clear() {
	if (m_surfaces.empty()) {
		return;
	}

	m_surfaces.clear();
	m_senderNames.clear();

	emit senderCountChanged(0);
	update();
}

void VideoGrid::paintEvent(QPaintEvent *) {
	QPainter painter(this);
	painter.fillRect(rect(), Qt::black);

	// Tested on what is drawable, not on whether any surface exists. A surface is created when a stream
	// is announced and stays blank until the first frame decodes, so "holds surfaces" and "has something
	// to draw" are different questions; conflating them let a blank surface reach the layout below with
	// a count of zero, and divide by it.
	const int count = tileCount();

	if (count == 0) {
		return;
	}

	// A roughly square arrangement, which is what every other video call looks like and needs no layout
	// configuration to be reasonable at any participant count.
	const int columns = static_cast< int >(std::ceil(std::sqrt(static_cast< double >(count))));
	const int rows    = (count + columns - 1) / columns;

	const int cellWidth  = width() / columns;
	const int cellHeight = height() / rows;

	if (cellWidth <= 0 || cellHeight <= 0) {
		return;
	}

	int index = 0;

	// Scaled to fit inside its cell without distorting it. Letterboxing is the honest presentation:
	// stretching somebody's screen share to fill a cell makes text unreadable.
	const auto drawInto = [&](const QImage &image, int slot, const QString &label) {
		const int column = slot % columns;
		const int row    = slot / columns;

		const QRect cell(column * cellWidth, row * cellHeight, cellWidth, cellHeight);
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
			QRect backdrop = painter.fontMetrics().boundingRect(labelRect, Qt::AlignBottom | Qt::AlignLeft, label);
			backdrop.adjust(-3, -1, 3, 1);

			painter.fillRect(backdrop, QColor(0, 0, 0, 140));

			painter.setPen(Qt::white);
			painter.drawText(labelRect, Qt::AlignBottom | Qt::AlignLeft, label);
		}
	};

	if (!m_selfFrame.isNull()) {
		drawInto(m_selfFrame, index++, tr("You"));
	}

	for (auto it = m_surfaces.cbegin(); it != m_surfaces.cend(); ++it) {
		const Surface &surface = it->second;

		if (surface.canvas.isNull()) {
			continue;
		}

		const auto nameIt = m_senderNames.find(surface.senderSession);
		QString label      = nameIt == m_senderNames.end() ? QString() : nameIt->second;

		// A camera tile is unlabelled beyond the name, matching today's behaviour. Anything else - a
		// screen or a window - is called out, since a sender showing a camera and a screen at once would
		// otherwise present as one person with two identical names.
		switch (surface.sourceKind) {
			case MumbleProto::VideoState_SourceKind_Display:
				label = label.isEmpty() ? tr("Screen") : tr("%1 (screen)").arg(label);
				break;
			case MumbleProto::VideoState_SourceKind_Window:
				label = label.isEmpty() ? tr("Window") : tr("%1 (window)").arg(label);
				break;
			case MumbleProto::VideoState_SourceKind_Application:
				label = label.isEmpty() ? tr("App") : tr("%1 (app)").arg(label);
				break;
			default:
				break;
		}

		drawInto(surface.canvas, index++, label);
	}
}
