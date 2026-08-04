// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "VideoConfigDialog.h"

#include "CameraVideoSource.h"
#include "VP8Codec.h"
#include "VideoEncoder.h"
#include "VideoSource.h"

#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>

const QString VideoConfigDialog::name = QLatin1String("VideoConfigDialog");

static ConfigWidget *VideoConfigDialogNew(Settings &st) {
	return new VideoConfigDialog(st);
}

// Just after the audio input and output pages, so video sits with the other media settings rather than
// somewhere the user has to hunt for it.
static ConfigRegistrar registrarVideoConfigDialog(1020, VideoConfigDialogNew);


VideoConfigDialog::VideoConfigDialog(Settings &st) : ConfigWidget(st) {
	buildUi();

	m_statsTimer.setInterval(1000);
	connect(&m_statsTimer, &QTimer::timeout, this, [this]() {
		if (!m_source) {
			return;
		}

		const double kbits = static_cast< double >(m_bytesThisSecond) * 8.0 / 1000.0;
		m_bytesThisSecond  = 0;

		m_stats->setText(tr("Approximately %1 kbit/s on the wire").arg(kbits, 0, 'f', 0));
	});
}

VideoConfigDialog::~VideoConfigDialog() {
	stopPreview();
}

QString VideoConfigDialog::title() const {
	return tr("Video");
}

const QString &VideoConfigDialog::getName() const {
	return VideoConfigDialog::name;
}

QIcon VideoConfigDialog::icon() const {
	return QIcon(QLatin1String("skin:config_basic.png"));
}

void VideoConfigDialog::buildUi() {
	auto *outer = new QVBoxLayout(this);

	auto *sourceBox    = new QGroupBox(tr("Source"), this);
	auto *sourceLayout = new QFormLayout(sourceBox);

	m_device = new QComboBox(sourceBox);
	m_device->setToolTip(tr("Which camera to share. Cameras are detected when this page opens."));
	sourceLayout->addRow(tr("&Camera"), m_device);

	m_resolution = new QComboBox(sourceBox);
	// Deliberately modest defaults. Resolution multiplies bandwidth and encoding cost quadratically, and
	// a call is not improved by a picture larger than the tile it is displayed in.
	m_resolution->addItem(tr("320 x 240 (QVGA)"), QSize(320, 240));
	m_resolution->addItem(tr("640 x 480 (VGA)"), QSize(640, 480));
	m_resolution->addItem(tr("1280 x 720 (720p)"), QSize(1280, 720));
	m_resolution->addItem(tr("1920 x 1080 (1080p)"), QSize(1920, 1080));
	sourceLayout->addRow(tr("&Resolution"), m_resolution);

	m_framerate = new QSpinBox(sourceBox);
	m_framerate->setRange(1, 60);
	m_framerate->setSuffix(tr(" fps"));
	m_framerate->setToolTip(tr("Frames per second. Bandwidth scales almost linearly with this, so halving "
							   "it is the cheapest way to fit a slow connection."));
	sourceLayout->addRow(tr("&Frame rate"), m_framerate);

	outer->addWidget(sourceBox);

	auto *codecBox    = new QGroupBox(tr("Codec"), this);
	auto *codecLayout = new QVBoxLayout(codecBox);

	m_codec = new QComboBox(codecBox);
	m_codec->addItem(tr("VP8 - best for cameras"), 0);
	m_codec->addItem(tr("Tiled image - best for screen sharing"), 1);
	m_codec->setToolTip(tr("VP8 compresses each frame against the previous one, which is what makes a "
						   "camera affordable. Tiled image sends only the parts of the picture that "
						   "changed, which costs nothing at all when the screen is still."));
	codecLayout->addWidget(m_codec);

	// VP8 controls.
	m_vp8Group      = new QWidget(codecBox);
	auto *vp8Layout = new QFormLayout(m_vp8Group);
	vp8Layout->setContentsMargins(0, 0, 0, 0);
	m_bitrate = new QSpinBox(m_vp8Group);
	m_bitrate->setRange(100, 8000);
	m_bitrate->setSingleStep(100);
	m_bitrate->setSuffix(tr(" kbit/s"));
	m_bitrate->setToolTip(tr("Target bitrate. The encoder holds close to this, so it is also what "
							 "everyone watching you will have to download."));
	vp8Layout->addRow(tr("&Bitrate"), m_bitrate);
	codecLayout->addWidget(m_vp8Group);

	// Tiled-image controls.
	m_tiledGroup      = new QWidget(codecBox);
	auto *tiledLayout = new QFormLayout(m_tiledGroup);
	tiledLayout->setContentsMargins(0, 0, 0, 0);
	m_tileQuality = new QSpinBox(m_tiledGroup);
	m_tileQuality->setRange(20, 95);
	m_tileQuality->setToolTip(tr("JPEG quality for each tile."));
	tiledLayout->addRow(tr("&Quality"), m_tileQuality);
	m_tileSize = new QComboBox(m_tiledGroup);
	m_tileSize->addItem(tr("64 px - finest, most overhead"), 64);
	m_tileSize->addItem(tr("128 px - recommended"), 128);
	m_tileSize->addItem(tr("192 px"), 192);
	m_tileSize->addItem(tr("256 px - coarsest"), 256);
	m_tileSize->setToolTip(tr("Smaller tiles resend less when a small area changes, at the cost of more "
							  "per-tile overhead."));
	tiledLayout->addRow(tr("&Tile size"), m_tileSize);
	codecLayout->addWidget(m_tiledGroup);

	outer->addWidget(codecBox);

	auto *previewBox    = new QGroupBox(tr("Preview"), this);
	auto *previewLayout = new QVBoxLayout(previewBox);

	m_preview = new VideoPreview(previewBox);
	previewLayout->addWidget(m_preview);

	m_stats = new QLabel(tr("Preview is off."), previewBox);
	previewLayout->addWidget(m_stats);

	m_previewButton = new QPushButton(tr("Start &preview"), previewBox);
	previewLayout->addWidget(m_previewButton);

	outer->addWidget(previewBox);
	outer->addStretch(1);

	connect(m_codec, QOverload< int >::of(&QComboBox::currentIndexChanged), this, &VideoConfigDialog::onCodecChanged);
	connect(m_previewButton, &QPushButton::clicked, this, &VideoConfigDialog::onPreviewToggled);
}

void VideoConfigDialog::refreshDevices() {
	const QString wanted = m_device->currentData().toString();

	m_device->clear();

	const QList< QCameraDevice > cameras = CameraVideoSource::availableCameras();

	if (cameras.isEmpty()) {
		// Said plainly rather than leaving an empty box, because an empty combo reads as a broken dialog
		// rather than as "this machine has no camera".
		m_device->addItem(tr("No camera detected"), QString());
		m_device->setEnabled(false);
		m_previewButton->setEnabled(false);

		return;
	}

	m_device->setEnabled(true);
	m_previewButton->setEnabled(true);

	for (const QCameraDevice &camera : cameras) {
		m_device->addItem(camera.description(), QString::fromUtf8(camera.id()));
	}

	const int index = m_device->findData(wanted);

	if (index >= 0) {
		m_device->setCurrentIndex(index);
	}
}

void VideoConfigDialog::onCodecChanged(int) {
	const bool isVP8 = m_codec->currentData().toInt() == 0;

	m_vp8Group->setVisible(isVP8);
	m_tiledGroup->setVisible(!isVP8);
}

void VideoConfigDialog::stopPreview() {
	m_statsTimer.stop();

	if (m_source) {
		m_source->stop();
		m_source.reset();
	}

	if (m_preview) {
		m_preview->clear();
	}

	if (m_previewButton) {
		m_previewButton->setText(tr("Start &preview"));
	}

	if (m_stats) {
		m_stats->setText(tr("Preview is off."));
	}
}

void VideoConfigDialog::onPreviewToggled() {
	if (m_source) {
		stopPreview();

		return;
	}

	const QString id = m_device->currentData().toString();

	QCameraDevice chosen;

	for (const QCameraDevice &camera : CameraVideoSource::availableCameras()) {
		if (QString::fromUtf8(camera.id()) == id) {
			chosen = camera;
			break;
		}
	}

	if (chosen.isNull()) {
		m_stats->setText(tr("That camera is no longer available."));

		return;
	}

	auto source = std::make_unique< CameraVideoSource >(chosen);

	connect(source.get(), &VideoSource::frameReady, this, &VideoConfigDialog::onFrameReady);
	connect(source.get(), &VideoSource::failed, this, [this](const QString &reason) {
		stopPreview();
		m_stats->setText(reason);
	});

	if (!source->start()) {
		m_stats->setText(tr("Could not start the camera."));

		return;
	}

	m_source          = std::move(source);
	m_bytesThisSecond = 0;
	m_statsTimer.start();
	m_previewButton->setText(tr("Stop &preview"));
	m_stats->setText(tr("Measuring..."));
}

void VideoConfigDialog::onFrameReady(const QImage &frame, std::uint64_t captureTimestampUsec) {
	const QSize resolution = m_resolution->currentData().toSize();
	const QImage scaled    = frame.scaled(resolution, Qt::KeepAspectRatio, Qt::SmoothTransformation);

	// Shown before anything else is attempted. Previously the preview was only updated from the decoded
	// output, so any frame the encoder declined to emit left the panel blank -- which is what a user saw
	// when nothing appeared at all.
	m_preview->setFrame(scaled);

	const int codec = m_codec->currentData().toInt();

	// Reconfigured only when a setting actually changes. setBitrate and setFramerate both tear the
	// encoder down, so calling them per frame rebuilt it every time: every frame became a keyframe, the
	// presentation clock restarted, and rate control never had a chance to settle.
	if (codec == 0) {
		const unsigned int bitrate   = static_cast< unsigned int >(m_bitrate->value());
		const unsigned int framerate = static_cast< unsigned int >(m_framerate->value());

		if (bitrate != m_vp8.bitrate() || framerate != m_vp8.framerate()) {
			m_vp8.setBitrate(bitrate);
			m_vp8.setFramerate(framerate);
		}

		for (const EncodedVideoUnit &unit : m_vp8.encode(scaled, 0, m_previewFrameNumber, captureTimestampUsec)) {
			m_bytesThisSecond += unit.payload.size();

			// The decoded picture is what a viewer really receives, so it is preferred when available.
			const QImage decoded = m_vp8Decoder.decode(unit.payload);

			if (!decoded.isNull()) {
				m_preview->setFrame(decoded);
			}
		}

		m_previewFrameNumber++;

		return;
	}

	if (m_tiled.tileSize() != m_tileSize->currentData().toInt() || m_tiled.quality() != m_tileQuality->value()) {
		m_tiled.setTileSize(m_tileSize->currentData().toInt());
		m_tiled.setQuality(m_tileQuality->value());
	}

	// Tiles are independently decodable and the source is already on screen, so only the cost needs
	// computing here. The frame number has to advance or dirty-tile detection compares a frame with
	// itself and reports nothing changed.
	for (const EncodedVideoUnit &unit : m_tiled.encode(scaled, 0, m_previewFrameNumber, captureTimestampUsec)) {
		m_bytesThisSecond += unit.payload.size();
	}

	m_previewFrameNumber++;
}

void VideoConfigDialog::load(const Settings &r) {
	refreshDevices();

	const int deviceIndex = m_device->findData(r.qsVideoDevice);

	if (deviceIndex >= 0) {
		m_device->setCurrentIndex(deviceIndex);
	}

	loadComboBox(m_codec, r.videoCodec);

	const int resolutionIndex = m_resolution->findData(QSize(r.iVideoWidth, r.iVideoHeight));
	m_resolution->setCurrentIndex(resolutionIndex >= 0 ? resolutionIndex : 1);

	m_framerate->setValue(r.iVideoFramerate);
	m_bitrate->setValue(r.iVideoBitrate);
	m_tileQuality->setValue(r.iVideoTileQuality);

	const int tileIndex = m_tileSize->findData(r.iVideoTileSize);
	m_tileSize->setCurrentIndex(tileIndex >= 0 ? tileIndex : 1);

	onCodecChanged(m_codec->currentIndex());
}

void VideoConfigDialog::save() const {
	s.qsVideoDevice = m_device->currentData().toString();
	s.videoCodec    = m_codec->currentData().toInt();

	const QSize resolution = m_resolution->currentData().toSize();
	s.iVideoWidth          = resolution.width();
	s.iVideoHeight         = resolution.height();

	s.iVideoFramerate   = m_framerate->value();
	s.iVideoBitrate     = m_bitrate->value();
	s.iVideoTileQuality = m_tileQuality->value();
	s.iVideoTileSize    = m_tileSize->currentData().toInt();
}
