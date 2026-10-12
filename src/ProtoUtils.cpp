// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "ProtoUtils.h"

namespace MumbleProto {

::Version::full_t getVersion(const MumbleProto::Version &msg) {
	if (msg.has_version_v2()) {
		return static_cast<::Version::full_t >(msg.version_v2());
	} else if (msg.has_version_v1()) {
		return ::Version::fromLegacyVersion(msg.version_v1());
	}

	return ::Version::UNKNOWN;
}

void setVersion(MumbleProto::Version &msg, const ::Version::full_t version) {
	msg.set_version_v2(version);
	msg.set_version_v1(::Version::toLegacyVersion(version));
}

::Version::full_t getSuggestedVersion(const MumbleProto::SuggestConfig &msg) {
	if (msg.has_version_v2()) {
		return static_cast<::Version::full_t >(msg.version_v2());
	} else if (msg.has_version_v1()) {
		return ::Version::fromLegacyVersion(msg.version_v1());
	}

	return ::Version::UNKNOWN;
}

void setSuggestedVersion(MumbleProto::SuggestConfig &msg, const ::Version::full_t version) {
	msg.set_version_v2(version);
	msg.set_version_v1(::Version::toLegacyVersion(version));
}

bool hasFeature(const google::protobuf::RepeatedField< int > &features, Feature feature) {
	for (const int advertised : features) {
		if (advertised == feature) {
			return true;
		}
	}

	return false;
}

bool isLegacyVideoPeer(::Version::full_t version, const QString &release) {
	return version == ::Version::fromComponents(1, 7, 0) && release == QLatin1String("1.7.0");
}

bool speaksVideo(const google::protobuf::RepeatedField< int > &features, ::Version::full_t version,
				 const QString &release) {
	if (!features.empty()) {
		return hasFeature(features, FEATURE_VIDEO);
	}

	return isLegacyVideoPeer(version, release);
}

} // namespace MumbleProto
