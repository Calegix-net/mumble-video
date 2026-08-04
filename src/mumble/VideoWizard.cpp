// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "VideoWizard.h"

#include "CameraVideoSource.h"
#include "Settings.h"
#include "VideoPreview.h"
#include "VideoSource.h"

#include <QtWidgets/QComboBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>

VideoWizard::VideoWizard(QWidget *parent) : QWizard(parent) {
	setWindowTitle(tr("Video Wizard"));
	setWizardStyle(QWizard::ModernStyle);
	setOption(QWizard::NoBackButtonOnStartPage, true);

	buildPages();

	connect(this, &QWizard::currentIdChanged, this, &VideoWizard::onPageChanged);
	connect(&m_probe, &VideoBandwidthProbe::candidateMeasured, this,
			[this](unsigned int, double) { m_progress->setValue(m_probe.progress()); });
	connect(&m_probe, &VideoBandwidthProbe::finished, this, &VideoWizard::finishMeasurement);
}

VideoWizard::~VideoWizard() {
	stopCamera();
}

void VideoWizard::buildPages() {
	// ---- Intro ----
	m_intro = new QWizardPage(this);
	m_intro->setTitle(tr("Video Setup"));
	m_intro->setSubTitle(tr("This will pick a camera, choose a quality level, and measure what it "
							"actually costs on your connection."));

	auto *introLayout = new QVBoxLayout(m_intro);
	introLayout->addWidget(
		new QLabel(tr("<p>Video is much more expensive than voice. A poorly chosen setting will not fail loudly; it "
					  "will simply make the call worse for everyone watching you.</p>"
					  "<p>Rather than asking you to guess, this wizard encodes real frames from your camera and "
					  "measures what each setting genuinely costs, then recommends one.</p>"
					  "<p>You can change any of it later under <b>Configure &rarr; Settings &rarr; Video</b>.</p>"),
				   m_intro));
	introLayout->addStretch(1);
	setPage(PageIntro, m_intro);

	// ---- Device ----
	m_devicePage = new QWizardPage(this);
	m_devicePage->setTitle(tr("Camera"));
	m_devicePage->setSubTitle(tr("Choose which camera to share. The preview confirms it works before you "
								 "rely on it in a call."));

	auto *deviceLayout = new QVBoxLayout(m_devicePage);
	m_device           = new QComboBox(m_devicePage);
	deviceLayout->addWidget(m_device);

	m_preview = new VideoPreview(m_devicePage);
	m_preview->setMinimumHeight(240);
	deviceLayout->addWidget(m_preview);

	m_deviceStatus = new QLabel(m_devicePage);
	deviceLayout->addWidget(m_deviceStatus);
	setPage(PageDevice, m_devicePage);

	connect(m_device, QOverload< int >::of(&QComboBox::currentIndexChanged), this, [this](int) {
		stopCamera();
		startCamera();
	});

	// ---- Quality ----
	m_qualityPage = new QWizardPage(this);
	m_qualityPage->setTitle(tr("What will you share?"));
	m_qualityPage->setSubTitle(tr("The two codecs are good at opposite things, so this choice matters "
								  "more than any of the numbers."));

	auto *qualityLayout = new QVBoxLayout(m_qualityPage);

	m_forCamera = new QRadioButton(tr("A camera, showing me or the room"), m_qualityPage);
	m_forCamera->setChecked(true);
	qualityLayout->addWidget(m_forCamera);
	qualityLayout->addWidget(
		new QLabel(tr("<i>Uses VP8, which compresses each frame against the one before it. Roughly half a megabit "
					  "per second at 480p.</i>"),
				   m_qualityPage));

	m_forScreen = new QRadioButton(tr("My screen, or a window"), m_qualityPage);
	qualityLayout->addWidget(m_forScreen);
	qualityLayout->addWidget(
		new QLabel(tr("<i>Uses tiled images, which send only the parts of the picture that changed. A still screen "
					  "costs nothing at all.</i>"),
				   m_qualityPage));

	auto *form   = new QFormLayout();
	m_resolution = new QComboBox(m_qualityPage);
	m_resolution->addItem(tr("320 x 240 (lowest bandwidth)"), QSize(320, 240));
	m_resolution->addItem(tr("640 x 480 (recommended)"), QSize(640, 480));
	m_resolution->addItem(tr("1280 x 720"), QSize(1280, 720));
	m_resolution->addItem(tr("1920 x 1080 (highest bandwidth)"), QSize(1920, 1080));
	m_resolution->setCurrentIndex(1);
	form->addRow(tr("Resolution"), m_resolution);

	m_framerate = new QSpinBox(m_qualityPage);
	m_framerate->setRange(5, 60);
	m_framerate->setValue(30);
	m_framerate->setSuffix(tr(" fps"));
	form->addRow(tr("Frame rate"), m_framerate);

	qualityLayout->addLayout(form);
	qualityLayout->addStretch(1);
	setPage(PageQuality, m_qualityPage);

	// ---- Bandwidth ----
	m_bandwidthPage = new QWizardPage(this);
	m_bandwidthPage->setTitle(tr("Measuring"));
	m_bandwidthPage->setSubTitle(tr("Encoding real frames at several settings to find what each one "
									"actually costs."));

	auto *bandwidthLayout = new QVBoxLayout(m_bandwidthPage);
	m_progress            = new QProgressBar(m_bandwidthPage);
	bandwidthLayout->addWidget(m_progress);

	m_measured = new QLabel(m_bandwidthPage);
	m_measured->setTextFormat(Qt::RichText);
	bandwidthLayout->addWidget(m_measured);

	auto *targetForm = new QFormLayout();
	m_target         = new QComboBox(m_bandwidthPage);
	targetForm->addRow(tr("Use"), m_target);
	bandwidthLayout->addLayout(targetForm);
	bandwidthLayout->addStretch(1);
	setPage(PageBandwidth, m_bandwidthPage);

	// ---- Done ----
	m_donePage = new QWizardPage(this);
	m_donePage->setTitle(tr("Ready"));

	auto *doneLayout = new QVBoxLayout(m_donePage);
	m_summary        = new QLabel(m_donePage);
	m_summary->setTextFormat(Qt::RichText);
	m_summary->setWordWrap(true);
	doneLayout->addWidget(m_summary);
	doneLayout->addStretch(1);
	setPage(PageDone, m_donePage);
}

std::unique_ptr< VideoSource > VideoWizard::createSource() {
	const QString id = m_device->currentData().toString();

	for (const QCameraDevice &camera : CameraVideoSource::availableCameras()) {
		if (QString::fromUtf8(camera.id()) == id) {
			return std::make_unique< CameraVideoSource >(camera);
		}
	}

	return nullptr;
}

bool VideoWizard::startCamera() {
	auto source = createSource();

	if (!source) {
		m_deviceStatus->setText(tr("No camera selected."));

		return false;
	}

	connect(source.get(), &VideoSource::frameReady, this, &VideoWizard::onFrameReady);
	connect(source.get(), &VideoSource::failed, this, [this](const QString &reason) {
		m_deviceStatus->setText(reason);
		stopCamera();
	});

	if (!source->start()) {
		m_deviceStatus->setText(tr("That camera could not be started. It may be in use by another "
								   "application."));

		return false;
	}

	m_source = std::move(source);
	m_deviceStatus->setText(tr("Waiting for the first frame..."));

	return true;
}

void VideoWizard::stopCamera() {
	m_probe.stop();

	if (m_source) {
		m_source->stop();
		m_source.reset();
	}
}

void VideoWizard::onPageChanged(int id) {
	if (id == PageDevice) {
		m_device->clear();

		const QList< QCameraDevice > cameras = CameraVideoSource::availableCameras();

		if (cameras.isEmpty()) {
			m_device->addItem(tr("No camera detected"), QString());
			m_deviceStatus->setText(tr("No camera was found. You can still finish this wizard and set "
									   "video up later, but nothing will be shared until a camera is "
									   "available."));

			return;
		}

		for (const QCameraDevice &camera : cameras) {
			m_device->addItem(camera.description(), QString::fromUtf8(camera.id()));
		}

		startCamera();

		return;
	}

	if (id == PageBandwidth) {
		beginMeasurement();

		return;
	}

	if (id == PageDone) {
		stopCamera();

		const QSize resolution = m_resolution->currentData().toSize();

		m_summary->setText(tr("<p><b>Camera:</b> %1<br/>"
							  "<b>Content:</b> %2<br/>"
							  "<b>Resolution:</b> %3 x %4 at %5 fps<br/>"
							  "<b>Bandwidth:</b> %6</p>"
							  "<p>These are saved when you press Finish, and can be changed at any time "
							  "under <b>Configure &rarr; Settings &rarr; Video</b>.</p>")
							   .arg(m_device->currentText())
							   .arg(m_forScreen->isChecked() ? tr("screen sharing") : tr("camera"))
							   .arg(resolution.width())
							   .arg(resolution.height())
							   .arg(m_framerate->value())
							   .arg(m_target->currentText()));

		return;
	}

	// Any other page: the camera is only needed for preview and measurement.
	if (id != PageDevice && id != PageBandwidth) {
		stopCamera();
	}
}

void VideoWizard::beginMeasurement() {
	m_measured->setText(QString());
	m_target->clear();

	if (m_forScreen->isChecked()) {
		// Tiled image has no bitrate target: its cost depends on how much of the screen changes, which a
		// camera cannot tell us. Measuring anyway would yield a confident number that means nothing.
		m_progress->setRange(0, 1);
		m_progress->setValue(1);
		m_measured->setText(tr("<p>Screen sharing sends only the regions that change, so its cost depends "
							   "on what is on your screen rather than on a setting. A still screen costs "
							   "nothing; scrolling text costs the most.</p>"
							   "<p>No measurement is useful here, so quality is left at the default.</p>"));
		m_target->addItem(tr("Adaptive (tiled image)"), 0);

		return;
	}

	m_progress->setRange(0, 100);
	m_progress->setValue(0);

	// The preview source is handed over to the probe, which owns it for the duration of the sweep.
	stopCamera();

	if (!m_probe.start(createSource(), m_resolution->currentData().toSize(),
					   static_cast< unsigned int >(m_framerate->value()), { 300, 800, 2000 })) {
		m_progress->setRange(0, 1);
		m_progress->setValue(1);
		m_measured->setText(tr("<p>No camera is available, so nothing can be measured. A conservative "
							   "default will be used.</p>"));
		m_target->addItem(tr("800 kbit/s (default)"), 800);

		return;
	}

	m_measured->setText(tr("Measuring..."));
}

void VideoWizard::onFrameReady(const QImage &frame, std::uint64_t) {
	const QImage scaled =
		frame.scaled(m_resolution->currentData().toSize(), Qt::KeepAspectRatio, Qt::SmoothTransformation);

	m_preview->setFrame(scaled);
	m_deviceStatus->setText(tr("Camera is working: %1 x %2.").arg(scaled.width()).arg(scaled.height()));
}

void VideoWizard::finishMeasurement() {
	m_progress->setValue(100);

	const std::vector< double > &results       = m_probe.results();
	const std::vector< unsigned int > &targets = m_probe.candidates();

	QString rows;

	for (std::size_t i = 0; i < results.size() && i < targets.size(); ++i) {
		rows += tr("<tr><td>%1 kbit/s target</td><td align=right>&nbsp;%2 kbit/s actual</td></tr>")
					.arg(targets[i])
					.arg(results[i], 0, 'f', 0);

		m_target->addItem(tr("%1 kbit/s").arg(targets[i]), targets[i]);
	}

	m_measured->setText(tr("<p>Measured on your camera:</p><table>%1</table>"
						   "<p>800 kbit/s suits most connections. Choose less if your upload is slow or "
						   "several people will watch you at once, since the server sends your video to "
						   "each of them separately.</p>")
							.arg(rows));

	const int index = m_target->findData(800u);
	m_target->setCurrentIndex(index >= 0 ? index : 0);
}

void VideoWizard::applyTo(Settings &s) const {
	s.qsVideoDevice = m_device->currentData().toString();
	s.videoCodec    = m_forScreen->isChecked() ? 1 : 0;

	const QSize resolution = m_resolution->currentData().toSize();
	s.iVideoWidth          = resolution.width();
	s.iVideoHeight         = resolution.height();
	s.iVideoFramerate      = m_framerate->value();

	const unsigned int bitrate = m_target->currentData().toUInt();

	if (bitrate > 0) {
		s.iVideoBitrate = static_cast< int >(bitrate);
	}

	s.videoWizardShown = true;
}

void VideoWizard::accept() {
	stopCamera();

	QWizard::accept();
}

void VideoWizard::reject() {
	stopCamera();

	QWizard::reject();
}
