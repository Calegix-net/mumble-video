// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

// Who is treated as speaking video. Getting this wrong in one direction costs a stock Mumble client its
// UDP voice, since the server then frames its datagrams for video; in the other it costs one of this
// fork's own older releases its video.

#include "Mumble.pb.h"
#include "ProtoUtils.h"
#include "Version.h"

#include <QObject>
#include <QtTest>

class TestFeatureNegotiation : public QObject {
	Q_OBJECT
private slots:
	void anAdvertisementIsReadAsIs();
	void releasesFromBeforeTheAdvertisementAreRecognised();
	void stockMumbleIsNotMistakenForThem();
	void anAdvertisementOverridesTheVersion();
};

void TestFeatureNegotiation::anAdvertisementIsReadAsIs() {
	MumbleProto::Authenticate authenticate;
	QVERIFY(!MumbleProto::hasFeature(authenticate.features(), MumbleProto::FEATURE_VIDEO));

	authenticate.add_features(MumbleProto::FEATURE_FILE_TRANSFER);
	QVERIFY(!MumbleProto::hasFeature(authenticate.features(), MumbleProto::FEATURE_VIDEO));

	authenticate.add_features(MumbleProto::FEATURE_VIDEO);
	QVERIFY(MumbleProto::hasFeature(authenticate.features(), MumbleProto::FEATURE_VIDEO));
	QVERIFY(MumbleProto::speaksVideo(authenticate.features(), Version::fromComponents(1, 5, 0),
									 QStringLiteral("anything")));
}

void TestFeatureNegotiation::releasesFromBeforeTheAdvertisementAreRecognised() {
	// v1.7.0 to v1.9.1 all report this, and advertise nothing.
	const MumbleProto::Authenticate nothingAdvertised;

	QVERIFY(MumbleProto::isLegacyVideoPeer(Version::fromComponents(1, 7, 0), QStringLiteral("1.7.0")));
	QVERIFY(MumbleProto::speaksVideo(nothingAdvertised.features(), Version::fromComponents(1, 7, 0),
									 QStringLiteral("1.7.0")));
}

void TestFeatureNegotiation::stockMumbleIsNotMistakenForThem() {
	const MumbleProto::Authenticate nothingAdvertised;

	// Upstream releases, which carry a build number in the patch field.
	QVERIFY(!MumbleProto::speaksVideo(nothingAdvertised.features(), Version::fromComponents(1, 5, 735),
									  QStringLiteral("1.5.735")));
	QVERIFY(!MumbleProto::speaksVideo(nothingAdvertised.features(), Version::fromComponents(1, 7, 12),
									  QStringLiteral("1.7.12")));

	// Both have to match, not either.
	QVERIFY(!MumbleProto::isLegacyVideoPeer(Version::fromComponents(1, 7, 0), QStringLiteral("1.7.0 (custom)")));
	QVERIFY(!MumbleProto::isLegacyVideoPeer(Version::fromComponents(1, 7, 1), QStringLiteral("1.7.0")));

	// A client that sent no Version at all.
	QVERIFY(!MumbleProto::speaksVideo(nothingAdvertised.features(), Version::UNKNOWN, QString()));
}

void TestFeatureNegotiation::anAdvertisementOverridesTheVersion() {
	// A peer that lists features has said what it speaks, even if it happens to report 1.7.0.
	MumbleProto::Authenticate authenticate;
	authenticate.add_features(MumbleProto::FEATURE_FILE_TRANSFER);

	QVERIFY(
		!MumbleProto::speaksVideo(authenticate.features(), Version::fromComponents(1, 7, 0), QStringLiteral("1.7.0")));
}

QTEST_MAIN(TestFeatureNegotiation)
#include "TestFeatureNegotiation.moc"
