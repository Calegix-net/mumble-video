// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license.
#include "PipeWireScreenVideoSource.h"
#include "PortalScreenCast.h"
#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVirtualObject>
#include <QProcess>
#include <QSignalSpy>
#include <QtTest>
#include <climits>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>

class SourceProbe : public PipeWireScreenVideoSource {
public:
	SourceProbe() : PipeWireScreenVideoSource(PortalScreenCast::SourceType::Any, true) {}
	using PipeWireScreenVideoSource::publishFrame;
	using PipeWireScreenVideoSource::reportPersistentDrop;
	void begin() { m_running = true; }
	bool published() const { return m_everPublished; }
	bool deliveryQueued() const { return m_deliveryQueued; }
	int drops() const { return m_consecutiveDrops; }
};

// A private-bus portal with immediate responses, one supported source/cursor mode,
// and a deliberately pending picker. Runs in a child process so blocking D-Bus
// method replies cannot deadlock Qt's test thread.
class FakePortal : public QDBusVirtualObject {
public:
	int requestCloses = 0, sessionCloses = 0;
	uint types = 0, cursor = 0;
	QString session;
	QString pendingRequest;
	int starts = 0;
	QString introspect(const QString &) const override {
		return QStringLiteral(R"(<interface name="org.freedesktop.portal.ScreenCast">
   <property name="version" type="u" access="read"/>
   <property name="AvailableSourceTypes" type="u" access="read"/>
   <property name="AvailableCursorModes" type="u" access="read"/>
   <method name="CreateSession"><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
   <method name="SelectSources"><arg type="o" direction="in"/><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
   <method name="Start"><arg type="o" direction="in"/><arg type="s" direction="in"/><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
  </interface>)");
	}
	bool handleMessage(const QDBusMessage &message, const QDBusConnection &bus) override {
		const auto method = message.member();
		if (message.interface() == "org.freedesktop.DBus.Properties") {
			if (method == "GetAll")
				bus.send(message.createReply(QVariantList{ QVariantMap{
					{ "version", 5u }, { "AvailableSourceTypes", 1u }, { "AvailableCursorModes", 1u } } }));
			else
				bus.send(message.createReply(QVariantList{
					QVariant::fromValue(QDBusVariant(message.arguments().last().toString() == "version" ? 5u : 1u)) }));
			return true;
		}
		if (method == "Stats") {
			bus.send(message.createReply(QVariantList{ QVariantMap{ { "requests", requestCloses },
																	{ "sessions", sessionCloses },
																	{ "types", types },
																	{ "starts", starts },
																	{ "cursor", cursor } } }));
			return true;
		}
		if (method == "LateResponse") {
			bus.send(message.createReply());
			auto signal = QDBusMessage::createSignal(pendingRequest, "org.freedesktop.portal.Request", "Response");
			signal << 2u << QVariantMap();
			bus.send(signal);
			return true;
		}
		if (method == "Revoke") {
			bus.send(message.createReply());
			auto signal = QDBusMessage::createSignal(session, "org.freedesktop.portal.Session", "Closed");
			signal << QVariantMap();
			bus.send(signal);
			return true;
		}
		if (method == "Close") {
			if (message.interface() == "org.freedesktop.portal.Request")
				++requestCloses;
			else
				++sessionCloses;
			bus.send(message.createReply());
			return true;
		}
		if (method != "CreateSession" && method != "SelectSources" && method != "Start")
			return false;
		auto options   = qdbus_cast< QVariantMap >(message.arguments().last());
		QString sender = message.service().mid(1);
		sender.replace('.', '_');
		auto path = QString("/org/freedesktop/portal/desktop/request/%1/%2")
						.arg(sender, options.value("handle_token").toString());
		bus.send(message.createReply(QVariantList{ QVariant::fromValue(QDBusObjectPath(path)) }));
		QVariantMap results;
		if (method == "CreateSession") {
			session = QString("/org/freedesktop/portal/desktop/session/%1/%2")
						  .arg(sender, options.value("session_handle_token").toString());
			results.insert("session_handle", QVariant::fromValue(QDBusObjectPath(session)));
		} else if (method == "SelectSources") {
			types  = options.value("types").toUInt();
			cursor = options.value("cursor_mode").toUInt();
		} else {
			pendingRequest = path;
			++starts;
			return true; // Start waits for user input.
		}
		auto signal = QDBusMessage::createSignal(path, "org.freedesktop.portal.Request", "Response");
		signal << 0u << results;
		bus.send(signal);
		return true;
	}
};
class TestLinuxScreenCapture : public QObject {
	Q_OBJECT
	QProcess portal;
	QVariantMap stats() {
		auto message     = QDBusMessage::createMethodCall("org.freedesktop.portal.Desktop", "/org/mumble/Test",
														  "org.mumble.Test", "Stats");
		const auto reply = QDBusConnection::sessionBus().call(message);
		return reply.arguments().isEmpty() ? QVariantMap() : qdbus_cast< QVariantMap >(reply.arguments().first());
	}
private slots:
	void initTestCase() {
		portal.start(QCoreApplication::applicationFilePath(), { "--portal-service" });
		QVERIFY(portal.waitForStarted());
		QVERIFY(portal.waitForReadyRead());
	}
	void cleanupTestCase() {
		portal.terminate();
		QVERIFY(portal.waitForFinished());
	}
	void paddedRowsAndOffset() {
		unsigned char bytes[24] = {};
		bytes[4]                = 10;
		bytes[5]                = 20;
		bytes[6]                = 30;
		bytes[16]               = 40;
		bytes[17]               = 50;
		bytes[18]               = 60;
		spa_chunk chunk{};
		chunk.offset = 4;
		chunk.stride = 12;
		chunk.size   = 20;
		spa_data data{};
		data.data    = bytes;
		data.maxsize = 24;
		data.chunk   = &chunk;
		spa_buffer buffer{};
		buffer.n_datas = 1;
		buffer.datas   = &data;
		auto frame     = SourceProbe::imageFromBuffer(&buffer, QSize(2, 2), SPA_VIDEO_FORMAT_BGRA);
		QCOMPARE(frame.size(), QSize(2, 2));
		QCOMPARE(frame.pixel(0, 0), qRgb(30, 20, 10));
		QCOMPARE(frame.pixel(0, 1), qRgb(60, 50, 40));
		chunk.offset = 28;
		QCOMPARE(SourceProbe::imageFromBuffer(&buffer, QSize(2, 2), SPA_VIDEO_FORMAT_RGBA).pixel(0, 0),
				 qRgb(10, 20, 30));
	}
	void invalidBuffers() {
		unsigned char bytes[16] = {};
		spa_chunk chunk{};
		chunk.size   = 16;
		chunk.stride = 8;
		spa_data data{};
		data.data    = bytes;
		data.maxsize = 16;
		data.chunk   = &chunk;
		spa_buffer buffer{};
		buffer.n_datas = 1;
		buffer.datas   = &data;
		chunk.stride   = INT_MAX;
		QVERIFY(SourceProbe::imageFromBuffer(&buffer, QSize(2, 5), SPA_VIDEO_FORMAT_BGRA).isNull());
		chunk.stride = 8;
		chunk.offset = 12;
		QVERIFY(SourceProbe::imageFromBuffer(&buffer, QSize(2, 2), SPA_VIDEO_FORMAT_BGRA).isNull());
		chunk.offset = 0;
		chunk.size   = 15;
		QVERIFY(SourceProbe::imageFromBuffer(&buffer, QSize(2, 2), SPA_VIDEO_FORMAT_BGRA).isNull());
		chunk.size  = 16;
		chunk.flags = SPA_CHUNK_FLAG_CORRUPTED;
		QVERIFY(SourceProbe::imageFromBuffer(&buffer, QSize(2, 2), SPA_VIDEO_FORMAT_BGRA).isNull());
		chunk.flags = 0;
		QVERIFY(SourceProbe::imageFromBuffer(&buffer, QSize(2, 2), SPA_VIDEO_FORMAT_NV12).isNull());
		data.maxsize = 0;
		QVERIFY(SourceProbe::imageFromBuffer(&buffer, QSize(2, 2), SPA_VIDEO_FORMAT_BGRA).isNull());
	}
	void emptyBuffersDoNotExposeOldPixels() {
		unsigned char bytes[16];
		memset(bytes, 255, sizeof(bytes));
		spa_chunk chunk{};
		chunk.size   = 16;
		chunk.stride = 8;
		chunk.flags  = SPA_CHUNK_FLAG_EMPTY;
		spa_data data{};
		data.data    = bytes;
		data.maxsize = 16;
		data.chunk   = &chunk;
		spa_buffer buffer{};
		buffer.n_datas   = 1;
		buffer.datas     = &data;
		const auto frame = SourceProbe::imageFromBuffer(&buffer, QSize(2, 2), SPA_VIDEO_FORMAT_BGRA);
		QCOMPARE(frame.pixel(0, 0), qRgb(0, 0, 0));
	}
	void restartDiscardsOldFramesAndErrors() {
		SourceProbe source;
		source.begin();
		QSignalSpy frames(&source, &VideoSource::frameReady);
		QSignalSpy errors(&source, &VideoSource::failed);
		QImage image(2, 2, QImage::Format_ARGB32);
		image.fill(Qt::red);
		for (int i = 0; i < 100; ++i)
			source.publishFrame(image, 1);
		QVERIFY(source.deliveryQueued());
		QVERIFY(source.published());
		for (int i = 0; i < 30; ++i)
			source.reportPersistentDrop("old capture failure");
		source.stop();
		source.begin();
		QVERIFY(!source.published());
		QCOMPARE(source.drops(), 0);
		QCoreApplication::processEvents();
		QCOMPARE(frames.count(), 0);
		QCOMPARE(errors.count(), 0);
		image.fill(Qt::blue);
		source.publishFrame(image, 2);
		QTRY_COMPARE(frames.count(), 1);
		QCOMPARE(qvariant_cast< QImage >(frames.first().first()).pixel(0, 0), qRgb(0, 0, 255));
	}
	void unsupportedNegotiationClearsGeometry() {
		SourceProbe source;
		source.begin();
		QSignalSpy errors(&source, &VideoSource::failed);
		uint8_t bytes[256];
		spa_pod_builder builder = SPA_POD_BUILDER_INIT(bytes, sizeof(bytes));
		spa_video_info_raw info{};
		info.format = SPA_VIDEO_FORMAT_NV12;
		info.size   = SPA_RECTANGLE(8, 8);
		source.onStreamParamChanged(SPA_PARAM_Format, spa_format_video_raw_build(&builder, SPA_PARAM_Format, &info));
		QTRY_COMPARE(errors.count(), 1);
		QVERIFY(!source.isRunning());
	}
	void portalCapabilitiesAndCancellation() {
		PortalScreenCast source;
		QVERIFY2(source.requestAccess(PortalScreenCast::SourceType::Any, true),
				 portal.readAllStandardError().constData());
		QTRY_COMPARE(stats().value("types").toUInt(), 1u);
		QCOMPARE(stats().value("cursor").toUInt(), 1u);
		const auto before = stats().value("requests").toInt();
		source.close();
		QTRY_VERIFY(stats().value("requests").toInt() > before);
	}
	void cancelledPickerIgnoresLateResponse() {
		PortalScreenCast source;
		QSignalSpy errors(&source, &PortalScreenCast::failed);
		const auto before = stats().value("starts").toInt();
		QVERIFY(source.requestAccess(PortalScreenCast::SourceType::Any, true));
		QTRY_VERIFY(stats().value("starts").toInt() > before);
		source.close();
		auto message = QDBusMessage::createMethodCall("org.freedesktop.portal.Desktop", "/org/mumble/Test",
													  "org.mumble.Test", "LateResponse");
		QDBusConnection::sessionBus().call(message);
		QTest::qWait(50);
		QCOMPARE(errors.count(), 0);
	}
	void disconnectedStreamStops() {
		SourceProbe source;
		source.begin();
		QSignalSpy errors(&source, &VideoSource::failed);
		source.onStreamStateChanged(PW_STREAM_STATE_UNCONNECTED, nullptr);
		QTRY_COMPARE(errors.count(), 1);
		QVERIFY(!source.isRunning());
	}
	void desktopRevocationEndsCapture() {
		PortalScreenCast source;
		QSignalSpy errors(&source, &PortalScreenCast::failed);
		const auto before = stats().value("starts").toInt();
		QVERIFY2(source.requestAccess(PortalScreenCast::SourceType::Any, true),
				 portal.readAllStandardError().constData());
		QTRY_VERIFY(stats().value("starts").toInt() > before);
		auto message = QDBusMessage::createMethodCall("org.freedesktop.portal.Desktop", "/org/mumble/Test",
													  "org.mumble.Test", "Revoke");
		QDBusConnection::sessionBus().call(message);
		QTRY_COMPARE(errors.count(), 1);
	}
};
int main(int argc, char **argv) {
	QCoreApplication app(argc, argv);
	if (app.arguments().contains("--portal-service")) {
		FakePortal fake;
		auto bus = QDBusConnection::sessionBus();
		if (!bus.registerVirtualObject("/", &fake, QDBusConnection::SubPath)
			|| !bus.registerService("org.freedesktop.portal.Desktop"))
			return 2;
		puts("READY");
		fflush(stdout);
		return app.exec();
	}
	TestLinuxScreenCapture test;
	return QTest::qExec(&test, argc, argv);
}
#include "TestLinuxScreenCapture.moc"
