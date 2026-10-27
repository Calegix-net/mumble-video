// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "LinuxSharePickerDialog.h"

#ifdef USE_PULSEAUDIO
#	include "PulseLoopbackSource.h"
#endif

#include <QtGui/QGuiApplication>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>

namespace {

/// The last choices made, for the next share in this session.
struct Remembered {
	bool window       = false;
	bool includeAudio = true;
	QString audioKey;
	bool showCursor = true;
};

Remembered &remembered() {
	static Remembered choices;
	return choices;
}

} // namespace

LinuxSharePickerDialog::LinuxSharePickerDialog(QWidget *parent, bool directCapture)
	: QDialog(parent), m_directCapture(directCapture) {
	setWindowTitle(tr("Share Screen"));
	setMinimumWidth(460);

	auto *layout = new QVBoxLayout(this);

	auto *what       = new QGroupBox(tr("What to share"), this);
	auto *whatLayout = new QVBoxLayout(what);
	m_screen         = new QRadioButton(tr("An entire screen"), what);
	m_region         = new QRadioButton(tr("Part of a screen"), what);
	m_window         = new QRadioButton(tr("A window"), what);
	whatLayout->addWidget(m_screen);
	whatLayout->addWidget(m_region);
	whatLayout->addWidget(m_window);

	// Offered only when this client grabs the screen itself. Through the portal it is the desktop that
	// decides which screen is captured, and a Wayland client cannot put a window on a chosen output, so
	// there is nothing to dim and drag a box over. Shown disabled with the reason rather than hidden,
	// because an option that silently appears on one machine and not another is worse than one that
	// explains itself.
	if (!m_directCapture) {
		m_region->setEnabled(false);
		m_region->setToolTip(tr("Not available when the desktop's own screen picker is used."));
	}
	// All three, including the region. Qt's own exclusivity only pairs auto-exclusive siblings that are
	// in no QButtonGroup, so a radio left out of the group here has nothing to be exclusive with: it
	// could be checked at the same time as "An entire screen", and picking a different option would not
	// clear it - leaving two radios lit and regionRequested() still true.
	auto *group = new QButtonGroup(this);
	group->addButton(m_screen);
	group->addButton(m_region);
	group->addButton(m_window);

	if (!m_directCapture) {
		auto *next = new QLabel(tr("Next, your desktop asks which screen or window, with a preview of each."), what);
		next->setWordWrap(true);
		QFont hintFont = next->font();
		hintFont.setItalic(true);
		next->setFont(hintFont);
		whatLayout->addWidget(next);
	} else {
		for (QScreen *screen : QGuiApplication::screens()) {
			m_screens << screen;
		}

		// No desktop picker follows on this path, so the question it would have asked is asked here.
		// With a single screen there is nothing to ask.
		if (m_screens.size() > 1) {
			auto *screenRow = new QHBoxLayout();
			m_screenChoice  = new QComboBox(what);

			for (const QPointer< QScreen > &screen : m_screens) {
				m_screenChoice->addItem(tr("%1 (%2x%3)")
											.arg(screen->name().isEmpty() ? tr("Screen") : screen->name())
											.arg(screen->geometry().width())
											.arg(screen->geometry().height()));
			}
			m_screenChoice->setCurrentIndex(
				std::max(0, static_cast< int >(m_screens.indexOf(QGuiApplication::primaryScreen()))));

			screenRow->addSpacing(20);
			screenRow->addWidget(new QLabel(tr("Screen:"), what));
			screenRow->addWidget(m_screenChoice, 1);
			whatLayout->addLayout(screenRow);
		}
	}
	layout->addWidget(what);

	m_includeAudio = new QCheckBox(tr("Include audio"), this);
	layout->addWidget(m_includeAudio);

	auto *audioRow   = new QHBoxLayout();
	m_audioFromLabel = new QLabel(tr("Audio from:"), this);
	m_audioFrom      = new QComboBox(this);
	m_audioFrom->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	m_audioFrom->setMinimumContentsLength(24);
	m_refreshAudioFrom = new QToolButton(this);
	m_refreshAudioFrom->setText(QStringLiteral("↻"));
	m_refreshAudioFrom->setToolTip(tr("Look again for applications playing sound"));
	audioRow->addSpacing(20);
	audioRow->addWidget(m_audioFromLabel);
	audioRow->addWidget(m_audioFrom, 1);
	audioRow->addWidget(m_refreshAudioFrom);
	layout->addLayout(audioRow);

	m_showCursor = new QCheckBox(tr("Show my cursor"), this);
	layout->addWidget(m_showCursor);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	buttons->button(QDialogButtonBox::Ok)->setText(tr("Share"));
	layout->addWidget(buttons);

	const Remembered &last = remembered();
	(last.window ? m_window : m_screen)->setChecked(true);
	m_includeAudio->setChecked(last.includeAudio);
	m_showCursor->setChecked(last.showCursor);

	if (m_directCapture) {
		m_screen->setChecked(true);
		m_window->setEnabled(false);
		m_window->setToolTip(tr("Sharing a single window needs the desktop's screen-cast portal, which this "
								"desktop does not provide."));
		m_showCursor->setChecked(false);
		m_showCursor->setEnabled(false);
		m_showCursor->setToolTip(tr("The cursor cannot be captured without the desktop's screen-cast portal."));
	}

#ifndef USE_PULSEAUDIO
	m_includeAudio->setChecked(false);
	m_includeAudio->setEnabled(false);
#else
	if (!PulseLoopbackSource::isAvailable()) {
		m_includeAudio->setChecked(false);
		m_includeAudio->setEnabled(false);
		m_includeAudio->setToolTip(tr("This system's audio server cannot record other applications."));
	}
#endif

	refreshApplications();

	connect(m_includeAudio, &QCheckBox::toggled, this, &LinuxSharePickerDialog::updateControls);
	connect(m_refreshAudioFrom, &QToolButton::clicked, this, &LinuxSharePickerDialog::refreshApplications);
	connect(buttons, &QDialogButtonBox::accepted, this, &LinuxSharePickerDialog::remember);
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	updateControls();
}

void LinuxSharePickerDialog::refreshApplications() {
	const QString previous = m_audioFrom->count() > 0 ? audioApplicationKey() : remembered().audioKey;

	m_audioFrom->clear();
	m_audioFrom->addItem(tr("All applications (except Mumble)"), QString());

#ifdef USE_PULSEAUDIO
	if (m_includeAudio->isEnabled()) {
		for (const PulseLoopbackSource::Application &application : PulseLoopbackSource::playingApplications()) {
			m_audioFrom->addItem(application.name, application.key);
		}
	}
#endif

	// A remembered application that is not playing right now stays selectable: it is still what was
	// chosen last time, and its sound is picked up as soon as it starts.
	int index = m_audioFrom->findData(previous);
	if (index < 0 && !previous.isEmpty()) {
		m_audioFrom->addItem(previous, previous);
		index = m_audioFrom->count() - 1;
	}
	m_audioFrom->setCurrentIndex(std::max(index, 0));
}

void LinuxSharePickerDialog::updateControls() {
	const bool audio = m_includeAudio->isEnabled() && m_includeAudio->isChecked();
	m_audioFromLabel->setEnabled(audio);
	m_audioFrom->setEnabled(audio);
	m_refreshAudioFrom->setEnabled(audio);
}

void LinuxSharePickerDialog::remember() {
	Remembered &last  = remembered();
	last.includeAudio = m_includeAudio->isChecked();
	last.audioKey     = audioApplicationKey();

	// Not choices on the direct-capture path, so they are left as the portal path last had them.
	if (!m_directCapture) {
		last.window     = m_window->isChecked();
		last.showCursor = m_showCursor->isChecked();
	}
}

PortalScreenCast::SourceType LinuxSharePickerDialog::sourceType() const {
	return m_window->isChecked() ? PortalScreenCast::SourceType::Window : PortalScreenCast::SourceType::Monitor;
}

QScreen *LinuxSharePickerDialog::captureScreen() const {
	if (!m_directCapture || m_screens.isEmpty()) {
		return nullptr;
	}

	const int index = m_screenChoice ? m_screenChoice->currentIndex() : 0;

	return index >= 0 && index < m_screens.size() ? m_screens.at(index).data() : nullptr;
}

bool LinuxSharePickerDialog::regionRequested() const {
	return m_region && m_region->isChecked() && m_region->isEnabled();
}

bool LinuxSharePickerDialog::includeAudio() const {
	return m_includeAudio->isEnabled() && m_includeAudio->isChecked();
}

QString LinuxSharePickerDialog::audioApplicationKey() const {
	return m_audioFrom->currentData().toString();
}

QString LinuxSharePickerDialog::audioApplicationName() const {
	return audioApplicationKey().isEmpty() ? QString() : m_audioFrom->currentText();
}

bool LinuxSharePickerDialog::showCursor() const {
	return m_showCursor->isChecked();
}
