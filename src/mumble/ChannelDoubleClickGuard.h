// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_CHANNELDOUBLECLICKGUARD_H_
#define MUMBLE_MUMBLE_CHANNELDOUBLECLICKGUARD_H_

#include "Settings.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QObject>
#include <QtCore/QPersistentModelIndex>
#include <QtCore/QPoint>

class QAbstractItemView;

/// Filters a tree's double-clicks down to the ones somebody meant.
///
/// A double-click on a channel moves you to it, so one that was not really aimed - the pointer slid
/// between the clicks, the tree scrolled or reshuffled under it, or the wheel was still spinning - moves
/// you out of the conversation you were in. Qt only checks that the two presses were close together in
/// time and space, so the rest is checked here: both clicks must land on the same row, the pointer must
/// not have travelled further than the platform drag threshold between them, and the second must not
/// come within quietAfterScrollMs of the view scrolling.
///
/// Install it on a view and connect to doubleClicked() instead of the view's own signal; it is emitted
/// exactly where the view's would be (so not for a double-click on an expand arrow), but only for
/// double-clicks that pass. The view's own handling - expanding a row on double-click - is untouched.
class ChannelDoubleClickGuard : public QObject {
private:
	Q_OBJECT
	Q_DISABLE_COPY(ChannelDoubleClickGuard)

public:
	/// How long after the view last scrolled a double-click is still taken as a stray one.
	static constexpr qint64 quietAfterScrollMs = 400;

	explicit ChannelDoubleClickGuard(QAbstractItemView *view);

	/// Whether a double-click at secondPos on secondRow, following a first press at firstPos on firstRow,
	/// was a deliberate one. msSinceScroll is negative when the view has never scrolled.
	static bool isDeliberate(const QPoint &firstPos, const QModelIndex &firstRow, const QPoint &secondPos,
							 const QModelIndex &secondRow, qint64 msSinceScroll, int dragThreshold);

	bool eventFilter(QObject *watched, QEvent *event) Q_DECL_OVERRIDE;

signals:
	void doubleClicked(const QModelIndex &index);

private:
	struct Press {
		QPoint pos;
		QPersistentModelIndex row;
		quint64 timestamp = 0;
		bool valid        = false;
	};

	QAbstractItemView *m_view;
	Press m_lastPress;
	Press m_previousPress;
	QElapsedTimer m_sinceScroll;
	/// Set while the view handles a double-click event that passed, so the view's doubleClicked signal
	/// (which carries no position of its own) can be forwarded for that one event only.
	bool m_pendingDeliberate = false;

	void onViewDoubleClicked(const QModelIndex &index);
};

/// What a double-click on a channel should do. Your own channel is never joined (there is nowhere to go),
/// and a mode this build does not know asks, the safe choice.
enum class ChannelDoubleClickAction { Ignore, Ask, Join };
ChannelDoubleClickAction channelDoubleClickAction(Settings::ChannelDoubleClick mode, bool isOwnChannel);

#endif // MUMBLE_MUMBLE_CHANNELDOUBLECLICKGUARD_H_
