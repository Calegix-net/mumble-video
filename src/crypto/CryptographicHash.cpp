// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "CryptographicHash.h"

#include "crypto/Digest.h"

#include <QtCore/QString>

class CryptographicHashPrivate {
public:
	CryptographicHashPrivate(Mumble::Crypto::DigestAlgorithm algorithm) : m_digest(algorithm) {}
	void addData(const QByteArray &buf);
	QByteArray result();

private:
	Mumble::Crypto::Digest m_digest;
};

void CryptographicHashPrivate::addData(const QByteArray &buf) {
	m_digest.addData(std::span< const std::uint8_t >(reinterpret_cast< const std::uint8_t * >(buf.constData()),
													 static_cast< std::size_t >(buf.size())));
}

QByteArray CryptographicHashPrivate::result() {
	const std::vector< std::uint8_t > digest = m_digest.result();
	return QByteArray(reinterpret_cast< const char * >(digest.data()), static_cast< qsizetype >(digest.size()));
}

QByteArray CryptographicHash::hash(const QByteArray &buf, Algorithm algo) {
	CryptographicHash h(algo);
	h.addData(buf);
	return h.result();
}

QString CryptographicHash::humanReadableAlgorithmName(CryptographicHash::Algorithm algo) {
	switch (algo) {
		case CryptographicHash::Sha1:
			return QLatin1String("SHA-1");
		case CryptographicHash::Sha256:
			return QLatin1String("SHA-256");
	}
	return QString();
}

QString CryptographicHash::shortAlgorithmName(CryptographicHash::Algorithm algo) {
	switch (algo) {
		case CryptographicHash::Sha1:
			return QLatin1String("sha1");
		case CryptographicHash::Sha256:
			return QLatin1String("sha256");
	}
	return QString();
}

CryptographicHash::CryptographicHash() : d(nullptr) {
}

CryptographicHash::CryptographicHash(CryptographicHash::Algorithm algo) : d(nullptr) {
	switch (algo) {
		case CryptographicHash::Sha1:
			d = new CryptographicHashPrivate(Mumble::Crypto::DigestAlgorithm::Sha1);
			break;
		case CryptographicHash::Sha256:
			d = new CryptographicHashPrivate(Mumble::Crypto::DigestAlgorithm::Sha256);
			break;
	}
}

void CryptographicHash::addData(const QByteArray &buf) {
	if (d) {
		d->addData(buf);
	}
}

QByteArray CryptographicHash::result() const {
	if (d) {
		return d->result();
	}
	return QByteArray();
}
