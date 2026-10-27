// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_VIDEOFRAMEDUMP_H_
#define MUMBLE_MUMBLE_VIDEOFRAMEDUMP_H_

#include <QtCore/QString>

#include <cstdint>

class QImage;

/**
 * Writes video frames to PNG files so that a benchmark can compare what was sent with what arrived.
 *
 * This exists because picture quality cannot be measured from the outside. Screenshotting the two
 * clients gives two pictures of two differently sized windows, taken at two moments, with the grid's own
 * labels drawn over them; comparing those says as much about the window manager as about the codec. What
 * a quality metric needs is the frame the encoder was handed and the frame the decoder produced, in the
 * same pixels, identified as the same frame - and only the client knows which frame is which.
 *
 * Frame numbers are the sender's, handed out by VideoEncodeCore and carried on every unit, so the
 * receiver names its files with the same numbers the sender did and a pair is matched by filename. The
 * benchmark then runs PSNR, SSIM and VMAF over the pairs offline.
 *
 * Nothing here runs unless the environment variable is set, and the directories are read once rather
 * than per frame, so an ordinary client pays one string comparison per frame for not measuring.
 */
namespace VideoFrameDump {

/// MUMBLE_VIDEO_DUMP_TX: where frames handed to the encoder are written, or empty when unset.
const QString &senderDirectory();

/// MUMBLE_VIDEO_DUMP_RX: where frames the decoder produced are written, or empty when unset.
const QString &receiverDirectory();

/// Writes @p image as `<directory>/<prefix>-<streamID>-<frameNumber>.png`, zero-padded so the files
/// sort in frame order. Failures are warned about once and otherwise ignored: this is measurement, and
/// a full disk is not a reason to stop a call.
void write(const QString &directory, const char *prefix, std::uint32_t streamID, std::uint64_t frameNumber,
		   const QImage &image);

} // namespace VideoFrameDump

#endif
