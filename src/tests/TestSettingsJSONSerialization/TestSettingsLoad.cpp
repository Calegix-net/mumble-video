// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.
#include "Settings.h"
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

class TestSettingsLoad : public QObject {
	Q_OBJECT
private slots:
	void malformedSchema_data() {
		QTest::addColumn< QByteArray >("json");
		QTest::newRow("missing-version") << QByteArray("{}");
		QTest::newRow("wrong-version-type") << QByteArray(R"({"settings_version":"bad"})");
		QTest::newRow("invalid-root") << QByteArray("[]");
		QTest::newRow("partial-settings") << QByteArray(R"({"settings_version":1,"audio":{"mute":true,"volume":{}}})");
	}
	void malformedSchema() {
		QFETCH(QByteArray, json);
		QTemporaryDir dir;
		QVERIFY(dir.isValid());
		QFile file(dir.filePath("settings.json"));
		QVERIFY(file.open(QIODevice::WriteOnly));
		QCOMPARE(file.write(json), json.size());
		file.close();
		Settings settings;
		settings.bMute    = false;
		const auto volume = settings.fVolume;
		settings.load(file.fileName(), true);
		QVERIFY(!settings.bMute);
		QCOMPARE(settings.fVolume, volume);
		QCOMPARE(settings.settingsLocation, file.fileName());
	}
	void schemaErrorLoadsBackup() {
		QTemporaryDir dir;
		const QString path = dir.filePath("settings.json");
		QFile invalid(path);
		QVERIFY(invalid.open(QIODevice::WriteOnly));
		invalid.write("{}");
		invalid.close();
		Settings backup;
		backup.bMute = true;
		backup.save(dir.filePath("backup.json"));
		QVERIFY(QFile::rename(dir.filePath("backup.json"), path + QStringLiteral(".back")));
		Settings settings;
		settings.bMute = false;
		settings.load(path, true);
		QVERIFY(settings.bMute);
		QCOMPARE(settings.settingsLocation, path);
	}
};
QTEST_MAIN(TestSettingsLoad)
#include "TestSettingsLoad.moc"
