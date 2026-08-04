// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_VIDEOWIZARD_H_
#define MUMBLE_MUMBLE_VIDEOWIZARD_H_

#include "VideoBandwidthProbe.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QTimer>
#include <QtGui/QImage>
#include <QtWidgets/QWizard>

#include <cstdint>
#include <memory>
#include <vector>

class QComboBox;
class QLabel;
class QProgressBar;
class QRadioButton;
class QSpinBox;
class VideoPreview;
class VideoSource;
struct Settings;

/**
 * Guided first-run setup for video, in the same spirit as the audio wizard.
 *
 * The settings page already exposes every knob, so a wizard that only re-presented them would be
 * decoration. What this does that the page cannot is answer the question a user has no way to answer
 * themselves: what bitrate should I actually use? It encodes real frames from the real camera at
 * several candidate settings, measures what each genuinely costs on the wire, and recommends one -
 * rather than asking somebody to guess a number whose consequences are invisible until a call goes
 * badly.
 */
class VideoWizard : public QWizard {
	Q_OBJECT
	Q_DISABLE_COPY(VideoWizard)

public:
	enum Page { PageIntro, PageDevice, PageQuality, PageBandwidth, PageDone };

	explicit VideoWizard(QWidget *parent = nullptr);
	~VideoWizard() override;

	void accept() override;
	void reject() override;

protected slots:
	void onPageChanged(int id);
	void onFrameReady(const QImage &frame, std::uint64_t captureTimestampUsec);

protected:
	// Intro
	QWizardPage *m_intro = nullptr;

	// Device
	QWizardPage *m_devicePage = nullptr;
	QComboBox *m_device       = nullptr;
	VideoPreview *m_preview   = nullptr;
	QLabel *m_deviceStatus    = nullptr;

	// Quality
	QWizardPage *m_qualityPage = nullptr;
	QRadioButton *m_forCamera  = nullptr;
	QRadioButton *m_forScreen  = nullptr;
	QComboBox *m_resolution    = nullptr;
	QSpinBox *m_framerate      = nullptr;

	// Bandwidth
	QWizardPage *m_bandwidthPage = nullptr;
	QProgressBar *m_progress     = nullptr;
	QLabel *m_measured           = nullptr;
	QComboBox *m_target          = nullptr;

	// Done
	QWizardPage *m_donePage = nullptr;
	QLabel *m_summary       = nullptr;

	std::unique_ptr< VideoSource > m_source;

	/// Owns the measurement. Kept separate so the calculation can be tested without a camera or a window.
	VideoBandwidthProbe m_probe;

public:
	/// Measured bitrates, in candidate order.
	const std::vector< double > &measuredBitrates() const { return m_probe.results(); }
	const std::vector< unsigned int > &candidateBitrates() const { return m_probe.candidates(); }
	bool isMeasuring() const { return m_probe.isRunning(); }

	/// Writes the chosen values into the given settings.
	///
	/// The wizard deliberately does not reach for the global settings itself: the caller decides where
	/// the result goes, which keeps this class free of the client's singleton and lets the outcome be
	/// checked directly.
	void applyTo(Settings &settings) const;

protected:
	void buildPages();

	/**
	 * Creates the capture source for the preview and the measurement sweep.
	 *
	 * Virtual so tests can substitute a synthetic source: the measurement arithmetic is the part of this
	 * class most worth testing and the part that hardware makes hardest to reach.
	 */
	virtual std::unique_ptr< VideoSource > createSource();

	bool startCamera();
	void stopCamera();
	void beginMeasurement();
	void finishMeasurement();
};

#endif // MUMBLE_MUMBLE_VIDEOWIZARD_H_
