// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_VIDEOCONFIGDIALOG_H_
#define MUMBLE_MUMBLE_VIDEOCONFIGDIALOG_H_

#include "ConfigDialog.h"
#include "VP8Codec.h"
#include "VideoEncoder.h"
#include "VideoPreview.h"

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
 * The video settings page.
 *
 * Deliberately built to mirror the audio pages: pick a device, pick a codec, then adjust the quality
 * knobs that codec actually has. The two codecs have genuinely different controls -- a bitrate target
 * means nothing to tiled JPEG, and a tile size means nothing to VP8 -- so the page shows whichever set
 * applies rather than presenting both and hoping the user knows which half is inert.
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
	void onCodecChanged(int index);
	void onPreviewToggled();
	void onFrameReady(const QImage &frame, std::uint64_t captureTimestampUsec);
	void refreshDevices();

protected:
	QComboBox *m_device     = nullptr;
	QComboBox *m_codec      = nullptr;
	QComboBox *m_resolution = nullptr;
	QSpinBox *m_framerate   = nullptr;

	QWidget *m_vp8Group = nullptr;
	QSpinBox *m_bitrate = nullptr;

	QWidget *m_tiledGroup   = nullptr;
	QSpinBox *m_tileQuality = nullptr;
	QComboBox *m_tileSize   = nullptr;

	QPushButton *m_previewButton = nullptr;
	VideoPreview *m_preview      = nullptr;
	QLabel *m_stats              = nullptr;

	std::unique_ptr< VideoSource > m_source;

	// Owned rather than function-static: statics would be shared between dialog instances and would
	// outlive the settings they were configured for.
	VP8Encoder m_vp8;
	VP8Decoder m_vp8Decoder;
	TiledImageEncoder m_tiled;
	std::uint64_t m_previewFrameNumber = 0;

	// Rolling measurement of what the current settings would actually cost on the wire, which is the
	// number that decides whether a setting is usable on a real connection.
	std::size_t m_bytesThisSecond = 0;
	QTimer m_statsTimer;

	void buildUi();
	void stopPreview();
};

#endif // MUMBLE_MUMBLE_VIDEOCONFIGDIALOG_H_
