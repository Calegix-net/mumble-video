// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "ChannelDoubleClickGuard.h"

#include <QtGui/QMouseEvent>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QApplication>
#include <QtWidgets/QScrollBar>

ChannelDoubleClickGuard::ChannelDoubleClickGuard(QAbstractItemView *view) : QObject(view), m_view(view) {
	view->viewport()->installEventFilter(this);

	// Every way the tree moves under the pointer - wheel, scrollbar, keyboard, autoscroll while dragging -
	// ends up as a scrollbar value change, so that is the one place to listen.
	auto noteScroll = [this]() { m_sinceScroll.start(); };
	connect(view->verticalScrollBar(), &QScrollBar::valueChanged, this, noteScroll);
	connect(view->horizontalScrollBar(), &QScrollBar::valueChanged, this, noteScroll);

	connect(view, &QAbstractItemView::doubleClicked, this, &ChannelDoubleClickGuard::onViewDoubleClicked);
}

bool ChannelDoubleClickGuard::isDeliberate(const QPoint &firstPos, const QModelIndex &firstRow,
										   const QPoint &secondPos, const QModelIndex &secondRow,
										   qint64 msSinceScroll, int dragThreshold) {
	if (msSinceScroll >= 0 && msSinceScroll < quietAfterScrollMs) {
		return false;
	}

	if (!firstRow.isValid() || !secondRow.isValid()
		|| firstRow.siblingAtColumn(0) != secondRow.siblingAtColumn(0)) {
		return false;
	}

	return (secondPos - firstPos).manhattanLength() <= dragThreshold;
}

bool ChannelDoubleClickGuard::eventFilter(QObject *watched, QEvent *event) {
	if (watched != m_view->viewport()) {
		return false;
	}

	if (event->type() == QEvent::MouseButtonPress) {
		auto *mouse = static_cast< QMouseEvent * >(event);
		if (mouse->button() == Qt::LeftButton) {
			m_previousPress = m_lastPress;
			m_lastPress     = { mouse->position().toPoint(), m_view->indexAt(mouse->position().toPoint()),
								mouse->timestamp(), true };
		}
	} else if (event->type() == QEvent::MouseButtonDblClick) {
		auto *mouse = static_cast< QMouseEvent * >(event);
		const QPoint pos = mouse->position().toPoint();

		// Some platforms deliver the second press as a press of its own just before the double-click
		// (with the same timestamp), others deliver only the double-click. Either way the click to compare
		// against is the one before this pair.
		const Press &first =
			(m_lastPress.valid && m_lastPress.timestamp == mouse->timestamp()) ? m_previousPress : m_lastPress;

		m_pendingDeliberate = mouse->button() == Qt::LeftButton && first.valid
							  && isDeliberate(first.pos, first.row, pos, m_view->indexAt(pos),
											  m_sinceScroll.isValid() ? m_sinceScroll.elapsed() : -1,
											  QApplication::startDragDistance());

		// A third click must not pair with the second as another double-click.
		m_lastPress = m_previousPress = Press();
	}

	return false;
}

void ChannelDoubleClickGuard::onViewDoubleClicked(const QModelIndex &index) {
	// The view emits this from inside its handling of the double-click event, which this filter saw first.
	const bool deliberate = m_pendingDeliberate;
	m_pendingDeliberate   = false;

	if (deliberate) {
		emit doubleClicked(index);
	}
}

ChannelDoubleClickAction channelDoubleClickAction(Settings::ChannelDoubleClick mode, bool isOwnChannel) {
	if (isOwnChannel) {
		return ChannelDoubleClickAction::Ignore;
	}

	switch (mode) {
		case Settings::ChannelDoubleClickJoin:
			return ChannelDoubleClickAction::Join;
		case Settings::ChannelDoubleClickNothing:
			return ChannelDoubleClickAction::Ignore;
		case Settings::ChannelDoubleClickAsk:
			break;
	}

	return ChannelDoubleClickAction::Ask;
}
