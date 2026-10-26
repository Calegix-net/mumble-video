// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

// Joining a channel by double-click: which double-clicks count, what each setting does with them, and that
// the setting survives a save and a value it does not know.

#include "ChannelDoubleClickGuard.h"
#include "Settings.h"

#include <QApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QScrollBar>
#include <QSignalSpy>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QTreeView>
#include <QtTest>

namespace {

/// A tree with enough rows to scroll, and the guard on it.
struct Tree {
	QStandardItemModel model;
	QTreeView view;
	ChannelDoubleClickGuard *guard;
	quint64 clock = 1000;

	Tree() {
		for (int i = 0; i < 100; ++i) {
			model.appendRow(new QStandardItem(QStringLiteral("Channel %1").arg(i)));
		}
		view.setModel(&model);
		// A double-click would otherwise start editing the item, and an editing view ignores the next one.
		view.setEditTriggers(QAbstractItemView::NoEditTriggers);
		view.resize(400, 200);
		guard = new ChannelDoubleClickGuard(&view);
		view.show();
		QApplication::processEvents();
	}

	QPoint centreOf(int row) const { return view.visualRect(model.index(row, 0)).center(); }

	void send(QEvent::Type type, const QPoint &pos, quint64 timestamp) {
		QMouseEvent event(type, pos, view.viewport()->mapToGlobal(pos), Qt::LeftButton,
						  type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
		event.setTimestamp(timestamp);
		QApplication::sendEvent(view.viewport(), &event);
	}

	/// The sequence a platform that reports only the double-click sends: press, release, double-click, release.
	void doubleClick(const QPoint &first, const QPoint &second) {
		send(QEvent::MouseButtonPress, first, clock += 10);
		send(QEvent::MouseButtonRelease, first, clock += 10);
		send(QEvent::MouseButtonDblClick, second, clock += 10);
		send(QEvent::MouseButtonRelease, second, clock += 10);
	}

	/// The sequence Qt 6 sends on most platforms: the second press arrives too, with the double-click's timestamp.
	void doubleClickWithSecondPress(const QPoint &first, const QPoint &second) {
		send(QEvent::MouseButtonPress, first, clock += 10);
		send(QEvent::MouseButtonRelease, first, clock += 10);
		clock += 10;
		send(QEvent::MouseButtonPress, second, clock);
		send(QEvent::MouseButtonDblClick, second, clock);
		send(QEvent::MouseButtonRelease, second, clock += 10);
	}
};

} // namespace

class TestChannelDoubleClick : public QObject {
	Q_OBJECT
private slots:
	void deliberateRules() {
		QStandardItemModel model;
		model.appendRow(new QStandardItem(QStringLiteral("a")));
		model.appendRow(new QStandardItem(QStringLiteral("b")));
		const QModelIndex a = model.index(0, 0);
		const QModelIndex b = model.index(1, 0);
		const QPoint p(50, 10);

		QVERIFY(ChannelDoubleClickGuard::isDeliberate(p, a, p, a, -1, 10));
		QVERIFY(ChannelDoubleClickGuard::isDeliberate(p, a, p + QPoint(6, 4), a, -1, 10));
		// Further than the drag threshold.
		QVERIFY(!ChannelDoubleClickGuard::isDeliberate(p, a, p + QPoint(8, 4), a, -1, 10));
		// The second click landed on another row.
		QVERIFY(!ChannelDoubleClickGuard::isDeliberate(p, a, p, b, -1, 10));
		QVERIFY(!ChannelDoubleClickGuard::isDeliberate(p, QModelIndex(), p, a, -1, 10));
		// Too soon after a scroll, and long enough after one.
		QVERIFY(!ChannelDoubleClickGuard::isDeliberate(p, a, p, a, 0, 10));
		QVERIFY(!ChannelDoubleClickGuard::isDeliberate(p, a, p, a, ChannelDoubleClickGuard::quietAfterScrollMs - 1, 10));
		QVERIFY(ChannelDoubleClickGuard::isDeliberate(p, a, p, a, ChannelDoubleClickGuard::quietAfterScrollMs, 10));
	}

	void actionPerMode() {
		QCOMPARE(channelDoubleClickAction(Settings::ChannelDoubleClickAsk, false), ChannelDoubleClickAction::Ask);
		QCOMPARE(channelDoubleClickAction(Settings::ChannelDoubleClickJoin, false), ChannelDoubleClickAction::Join);
		QCOMPARE(channelDoubleClickAction(Settings::ChannelDoubleClickNothing, false),
				 ChannelDoubleClickAction::Ignore);
		// Your own channel: nothing to join, in any mode.
		QCOMPARE(channelDoubleClickAction(Settings::ChannelDoubleClickAsk, true), ChannelDoubleClickAction::Ignore);
		QCOMPARE(channelDoubleClickAction(Settings::ChannelDoubleClickJoin, true), ChannelDoubleClickAction::Ignore);
		// A mode this build does not know asks (3 is the one value the enum can hold that it does not name).
		QCOMPARE(channelDoubleClickAction(static_cast< Settings::ChannelDoubleClick >(3), false),
				 ChannelDoubleClickAction::Ask);
	}

	void defaultIsAsk() { QCOMPARE(Settings().ceChannelDoubleClick, Settings::ChannelDoubleClickAsk); }

	void stillDoubleClickPasses() {
		Tree tree;
		QSignalSpy spy(tree.guard, &ChannelDoubleClickGuard::doubleClicked);

		tree.doubleClick(tree.centreOf(2), tree.centreOf(2));
		QCOMPARE(spy.count(), 1);
		QCOMPARE(spy.at(0).at(0).value< QModelIndex >().row(), 2);

		tree.doubleClickWithSecondPress(tree.centreOf(3), tree.centreOf(3) + QPoint(2, 1));
		QCOMPARE(spy.count(), 2);
		QCOMPARE(spy.at(1).at(0).value< QModelIndex >().row(), 3);
	}

	void secondClickOnAnotherRowIsIgnored() {
		Tree tree;
		QSignalSpy spy(tree.guard, &ChannelDoubleClickGuard::doubleClicked);

		// The view itself would take this one: its last press and the double-click are on the same row.
		tree.doubleClickWithSecondPress(tree.centreOf(2), tree.centreOf(3));
		QCOMPARE(spy.count(), 0);
	}

	void pointerMovedIsIgnored() {
		Tree tree;
		QSignalSpy spy(tree.guard, &ChannelDoubleClickGuard::doubleClicked);
		const QPoint first = tree.centreOf(2);
		const QPoint moved = first + QPoint(QApplication::startDragDistance() + 1, 0);
		QCOMPARE(tree.view.indexAt(moved).row(), 2);

		tree.doubleClickWithSecondPress(first, moved);
		QCOMPARE(spy.count(), 0);
		tree.doubleClick(first, moved);
		QCOMPARE(spy.count(), 0);
	}

	void doubleClickJustAfterScrollIsIgnored() {
		Tree tree;
		QSignalSpy spy(tree.guard, &ChannelDoubleClickGuard::doubleClicked);
		QVERIFY(tree.view.verticalScrollBar()->maximum() > 0);

		tree.view.verticalScrollBar()->setValue(5);
		tree.doubleClick(tree.centreOf(6), tree.centreOf(6));
		QCOMPARE(spy.count(), 0);

		QTest::qWait(static_cast< int >(ChannelDoubleClickGuard::quietAfterScrollMs) + 100);
		tree.doubleClick(tree.centreOf(6), tree.centreOf(6));
		QCOMPARE(spy.count(), 1);
	}

	void thirdClickDoesNotPairWithSecond() {
		Tree tree;
		QSignalSpy spy(tree.guard, &ChannelDoubleClickGuard::doubleClicked);

		tree.doubleClick(tree.centreOf(2), tree.centreOf(2));
		QCOMPARE(spy.count(), 1);
		// A double-click event with no fresh first press behind it.
		tree.send(QEvent::MouseButtonDblClick, tree.centreOf(2), tree.clock += 10);
		QCOMPARE(spy.count(), 1);
	}

	void settingRoundTripsAndToleratesUnknownValues() {
		QTemporaryDir dir;
		QVERIFY(dir.isValid());
		const QString path = dir.filePath(QStringLiteral("settings.json"));

		Settings saved;
		saved.ceChannelDoubleClick = Settings::ChannelDoubleClickNothing;
		saved.save(path);

		Settings loaded;
		loaded.load(path, true);
		QCOMPARE(loaded.ceChannelDoubleClick, Settings::ChannelDoubleClickNothing);

		// A spelling from some other build must leave the default in place, not stop the client.
		QFile file(path);
		QVERIFY(file.open(QIODevice::ReadOnly));
		QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
		file.close();
		QJsonObject ui = root.value(QStringLiteral("ui")).toObject();
		QCOMPARE(ui.value(QStringLiteral("channel_double_click")).toString(), QStringLiteral("DoNothing"));
		ui.insert(QStringLiteral("channel_double_click"), QStringLiteral("Sometimes"));
		root.insert(QStringLiteral("ui"), ui);
		QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
		file.write(QJsonDocument(root).toJson());
		file.close();

		Settings tolerant;
		tolerant.load(path, true);
		QCOMPARE(tolerant.ceChannelDoubleClick, Settings::ChannelDoubleClickAsk);
	}
};

QTEST_MAIN(TestChannelDoubleClick)
#include "TestChannelDoubleClick.moc"
