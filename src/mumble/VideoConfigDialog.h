// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_VIDEOCONFIGDIALOG_H_
#define MUMBLE_MUMBLE_VIDEOCONFIGDIALOG_H_

#include "ConfigDialog.h"
#include "VideoEncoder.h"
#include "VideoPreview.h"
#include "VpxCodec.h"

#include <QtCore/QTimer>
#include <QtGui/QImage>
#include <QtWidgets/QWidget>

#include <memory>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSlider;
class QSpinBox;
class VideoSource;

/**
 * The video settings page: the camera (device, resolution, frame rate, bitrate) and screen sharing
 * (bitrate, frame rate).
 *
 * There is no codec to choose any more. Cameras are always VP8 and screens always VP9 in its
 * screen-content mode (see VpxCodec.h); the tiled-JPEG codec this page used to offer as an alternative
 * cost several times the bandwidth for either. What is left are the two numbers that decide what a share
 * costs everyone watching it: its bitrate and its frame rate.
 */
class VideoConfigDialog : public ConfigWidget {
	Q_OBJECT
	Q_DISABLE_COPY(VideoConfigDialog)

public:
	explicit VideoConfigDialog(Settings &st);
	~VideoConfigDialog() override;

	QString title() const override;
	const QString &getName() const override;
	QIcon icon() const override;

	static const QString name;

public slots:
	void save() const override;
	void load(const Settings &r) override;

protected slots:
	void onPreviewToggled();
	void onFrameReady(const QImage &frame, std::uint64_t captureTimestampUsec);
	void refreshDevices();

protected:
	QComboBox *m_device     = nullptr;
	QComboBox *m_resolution = nullptr;
	QSpinBox *m_framerate   = nullptr;
	QSpinBox *m_bitrate     = nullptr;

	QComboBox *m_screenCodec     = nullptr;
	QSpinBox *m_screenBitrate    = nullptr;
	QSpinBox *m_screenVp9Bitrate = nullptr;
	QSpinBox *m_screenFramerate  = nullptr;

	QPushButton *m_previewButton = nullptr;
	VideoPreview *m_preview      = nullptr;
	QLabel *m_stats              = nullptr;

	std::unique_ptr< VideoSource > m_source;

	// Owned rather than function-static: statics would be shared between dialog instances and would
	// outlive the settings they were configured for.
	VpxEncoder m_vp8;
	VpxDecoder m_vp8Decoder;
	std::uint64_t m_previewFrameNumber = 0;

	// Rolling measurement of what the current settings would actually cost on the wire, which is the
	// number that decides whether a setting is usable on a real connection.
	std::size_t m_bytesThisSecond = 0;
	QTimer m_statsTimer;

	void buildUi();
	void stopPreview();
};

#endif // MUMBLE_MUMBLE_VIDEOCONFIGDIALOG_H_
