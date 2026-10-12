// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_PROTOUTILS_H_
#define MUMBLE_PROTOUTILS_H_

#include "Mumble.pb.h"
#include "Version.h"

#include <QString>

namespace MumbleProto {

::Version::full_t getVersion(const MumbleProto::Version &msg);
void setVersion(MumbleProto::Version &msg, const ::Version::full_t version);

::Version::full_t getSuggestedVersion(const MumbleProto::SuggestConfig &msg);
void setSuggestedVersion(MumbleProto::SuggestConfig &msg, const ::Version::full_t version);

/// Whether @p feature is among those a peer advertised, in Authenticate.features or ServerConfig.features.
bool hasFeature(const google::protobuf::RepeatedField< int > &features, Feature feature);

/**
 * Whether a peer that advertised no features at all is one of this fork's releases v1.7.0 to v1.9.1, which
 * speak video but predate the advertisement. They all report version 1.7.0 with the release string "1.7.0";
 * see the comment on Feature in Mumble.proto for what else can match and what that costs it.
 *
 * @param version The peer's version, from its Version message.
 * @param release The peer's release string, from the same message.
 */
bool isLegacyVideoPeer(::Version::full_t version, const QString &release);

/**
 * Whether a peer speaks video: it listed FEATURE_VIDEO, or it listed nothing and is a release from before
 * there was anything to list (isLegacyVideoPeer).
 */
bool speaksVideo(const google::protobuf::RepeatedField< int > &features, ::Version::full_t version,
				 const QString &release);

} // namespace MumbleProto

#endif // MUMBLE_PROTOUTILS_H_
