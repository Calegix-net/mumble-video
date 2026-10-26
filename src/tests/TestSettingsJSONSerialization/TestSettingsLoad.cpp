// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.
#include "Cert.h"
#include "Settings.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

#include <openssl/evp.h>
#include <openssl/opensslv.h>
#include <openssl/pkcs12.h>
#include <openssl/x509.h>
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#	include <openssl/err.h>
#	include <openssl/provider.h>
#endif

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
	// Settings that cannot be decoded as a whole still give up their certificate. The identity is what a
	// server's registrations are tied to, and the file is shared with Mumble: losing it here meant a new
	// certificate, "wrong certificate" from every server, and that new certificate saved over Mumble's.
	void undecodableSettingsKeepTheirCertificate() {
		const Settings::KeyPair identity = CertWizard::generateNewCert(QStringLiteral("salvage"));
		QVERIFY(CertWizard::validateCert(identity));

		QTemporaryDir dir;
		QVERIFY(dir.isValid());
		QFile file(dir.filePath("mumble_settings.json"));
		QVERIFY(file.open(QIODevice::WriteOnly));
		file.write(R"({"settings_version":1,"audio":{"mute":true,"volume":{}},"certificate":")");
		file.write(CertWizard::exportCert(identity).toBase64());
		file.write(R"("})");
		file.close();

		Settings settings;
		settings.load(file.fileName(), true);

		// Still all or nothing for everything else...
		QVERIFY(!settings.bMute);
		// ...but not for the identity.
		QVERIFY(CertWizard::validateCert(settings.kpCertificate));
		QCOMPARE(settings.kpCertificate.first.first().digest(), identity.first.first().digest());
	}

	// The certificate wizard's import accepts the other client's settings file, not only a PKCS12 export.
	void certificateImportsFromSettingsFile() {
		const Settings::KeyPair identity = CertWizard::generateNewCert(QStringLiteral("import"));

		Settings written;
		written.kpCertificate = identity;
		QTemporaryDir dir;
		const QString path = dir.filePath("mumble_settings.json");
		written.save(path);

		QFile file(path);
		QVERIFY(file.open(QIODevice::ReadOnly));
		const Settings::KeyPair imported = CertWizard::importCert(file.readAll());
		QVERIFY(CertWizard::validateCert(imported));
		QCOMPARE(imported.first.first().digest(), identity.first.first().digest());
		QCOMPARE(imported.second.toDer(), identity.second.toDer());

		// Something that is neither stays a refusal, rather than a crash or an empty identity accepted.
		QVERIFY(!CertWizard::validateCert(CertWizard::importCert(QByteArray(R"({"settings_version":1})"))));
		QVERIFY(!CertWizard::validateCert(CertWizard::importCert(QByteArray("{not json"))));
	}
	// An identity written by a Mumble built against OpenSSL 1.x: its PKCS12 certificate bag is encrypted
	// with RC2-40, which OpenSSL 3 only has in the legacy provider. This client could not read one at all,
	// so a regular Mumble's certificate - exported or straight from its settings - never came across.
	void legacyRc2CertificateImports() {
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
		// Made the way OpenSSL 1.x's PKCS12_create made it, in a library context of its own so that the
		// legacy provider loaded here does not leak into the default context the client parses in.
		OSSL_LIB_CTX *old = OSSL_LIB_CTX_new();
		OSSL_PROVIDER *oldLegacy  = OSSL_PROVIDER_load(old, "legacy");
		// Where the client looks after OpenSSL's own directory, so that a test binary placed in a bundle
		// layout (with the system module out of the way) checks the client's fallback as well.
		for (const char *relative : { "ossl-modules", "../lib/ossl-modules" }) {
			const QString dir = QDir(QCoreApplication::applicationDirPath()).filePath(QLatin1String(relative));
			if (oldLegacy || !QFileInfo(dir).isDir()) {
				continue;
			}
			OSSL_PROVIDER_set_default_search_path(old, QDir::toNativeSeparators(dir).toUtf8().constData());
			oldLegacy = OSSL_PROVIDER_load(old, "legacy");
		}
		OSSL_PROVIDER *oldDefault = OSSL_PROVIDER_load(old, "default");
		if (!oldLegacy || !oldDefault) {
			OSSL_LIB_CTX_free(old);
			// Not a skip: every build this client ships has the module, and the client itself needs it to
			// read such a file, so a build environment without it is a broken one.
			QFAIL("the OpenSSL legacy provider module could not be loaded");
		}

		EVP_PKEY *key = EVP_PKEY_Q_keygen(old, nullptr, "RSA", static_cast< size_t >(2048));
		X509 *cert    = X509_new_ex(old, nullptr);
		QVERIFY(key && cert);
		X509_set_version(cert, 2);
		ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
		X509_gmtime_adj(X509_getm_notBefore(cert), 0);
		X509_gmtime_adj(X509_getm_notAfter(cert), 30 * 24 * 3600);
		X509_set_pubkey(cert, key);
		X509_NAME_add_entry_by_txt(X509_get_subject_name(cert), "CN", MBSTRING_ASC,
								   reinterpret_cast< const unsigned char * >("legacy"), -1, -1, 0);
		X509_set_issuer_name(cert, X509_get_subject_name(cert));
		QVERIFY(X509_sign(cert, key, EVP_sha256()) > 0);

		PKCS12 *pkcs = PKCS12_create_ex("", "Mumble Identity", key, cert, nullptr, NID_pbe_WithSHA1And3_Key_TripleDES_CBC,
										NID_pbe_WithSHA1And40BitRC2_CBC, 0, 0, 0, old, nullptr);
		QVERIFY(pkcs);
		unsigned char *der = nullptr;
		const int length   = i2d_PKCS12(pkcs, &der);
		QVERIFY(length > 0);
		const QByteArray exported(reinterpret_cast< const char * >(der), length);
		OPENSSL_free(der);

		unsigned char *certDer = nullptr;
		const int certLength   = i2d_X509(cert, &certDer);
		const QByteArray expectedDigest =
			QSslCertificate(QByteArray(reinterpret_cast< const char * >(certDer), certLength), QSsl::Der).digest();
		OPENSSL_free(certDer);

		PKCS12_free(pkcs);
		X509_free(cert);
		EVP_PKEY_free(key);
		OSSL_PROVIDER_unload(oldLegacy);
		OSSL_PROVIDER_unload(oldDefault);
		OSSL_LIB_CTX_free(old);

		if (!OSSL_PROVIDER_available(nullptr, "legacy")) {
			// The situation the client was in: a plain parse cannot decrypt the certificate.
			const unsigned char *in = reinterpret_cast< const unsigned char * >(exported.constData());
			PKCS12 *parsed          = d2i_PKCS12(nullptr, &in, exported.size());
			QVERIFY(parsed);
			EVP_PKEY *parsedKey   = nullptr;
			X509 *parsedCert      = nullptr;
			STACK_OF(X509) *chain = nullptr;
			const int ok          = PKCS12_parse(parsed, nullptr, &parsedKey, &parsedCert, &chain);
			QVERIFY(!ok || !parsedCert);
			PKCS12_free(parsed);
			ERR_clear_error();
		}

		const Settings::KeyPair imported = CertWizard::importCert(exported);
		QVERIFY(CertWizard::validateCert(imported));
		QCOMPARE(imported.first.first().digest(), expectedDigest);

		// And it survives the trip through a settings file, which is how it arrives from a shared one.
		Settings written;
		written.kpCertificate = imported;
		QTemporaryDir dir;
		const QString path = dir.filePath("mumble_settings.json");
		written.save(path);
		Settings read;
		read.load(path, true);
		QCOMPARE(read.kpCertificate.first.first().digest(), expectedDigest);
#else
		QSKIP("built against OpenSSL 1.x, which reads RC2 natively");
#endif
	}
};
QTEST_MAIN(TestSettingsLoad)
#include "TestSettingsLoad.moc"
