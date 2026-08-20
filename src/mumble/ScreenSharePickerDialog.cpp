// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "ScreenSharePickerDialog.h"

#include <QtGui/QFont>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

namespace {

constexpr int KindRole  = Qt::UserRole;
constexpr int IndexRole = Qt::UserRole + 1;

/// A bold, unselectable row used to label the "Displays"/"Windows" groups within the one combined list.
QListWidgetItem *addSectionHeader(QListWidget *list, const QString &text) {
	auto *item = new QListWidgetItem(text, list);
	item->setFlags(Qt::NoItemFlags);

	QFont font = item->font();
	font.setBold(true);
	item->setFont(font);

	return item;
}

} // namespace

ScreenSharePickerDialog::ScreenSharePickerDialog(QWidget *parent) : QDialog(parent) {
	setWindowTitle(tr("Share Screen"));

	m_displays = DxgiDisplayVideoSource::availableDisplays();
	m_windows  = WgcWindowVideoSource::availableWindows();

	auto *layout = new QVBoxLayout(this);

	layout->addWidget(new QLabel(tr("Choose what to share:"), this));

	m_targetList = new QListWidget(this);
	layout->addWidget(m_targetList);

	m_includeAudio = new QCheckBox(tr("Include audio"), this);
	m_includeAudio->setChecked(true);
	layout->addWidget(m_includeAudio);

	m_windowAudioOnly = new QCheckBox(tr("Only this application's audio (not all system sound)"), this);
	m_windowAudioOnly->setChecked(false);
	layout->addWidget(m_windowAudioOnly);

	m_showCursor = new QCheckBox(tr("Show my cursor"), this);
	m_showCursor->setChecked(true);
	layout->addWidget(m_showCursor);

	m_buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	layout->addWidget(m_buttonBox);

	populateList();

	connect(m_targetList, &QListWidget::currentRowChanged, this, &ScreenSharePickerDialog::onCurrentRowChanged);

	// windowAudioOnly's availability also depends on whether audio is being included at all, so toggling
	// that box needs to re-run the same enabled-state logic as changing the selection does.
	connect(m_includeAudio, &QCheckBox::toggled, this, [this](bool) { onCurrentRowChanged(m_targetList->currentRow()); });

	connect(m_buttonBox, &QDialogButtonBox::accepted, this, &ScreenSharePickerDialog::onAccepted);
	connect(m_buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

	onCurrentRowChanged(m_targetList->currentRow());
}

void ScreenSharePickerDialog::populateList() {
	m_targetList->clear();

	if (!m_displays.isEmpty()) {
		addSectionHeader(m_targetList, tr("Displays"));

		for (int i = 0; i < m_displays.size(); ++i) {
			auto *item = new QListWidgetItem(m_displays.at(i).friendlyName, m_targetList);
			item->setData(KindRole, static_cast< int >(ScreenShareTargetKind::Display));
			item->setData(IndexRole, i);
		}
	}

	if (!m_windows.isEmpty()) {
		addSectionHeader(m_targetList, tr("Windows"));

		for (int i = 0; i < m_windows.size(); ++i) {
			auto *item = new QListWidgetItem(m_windows.at(i).title, m_targetList);
			item->setData(KindRole, static_cast< int >(ScreenShareTargetKind::Window));
			item->setData(IndexRole, i);
		}
	}

	// Selects the first real (non-header) entry, if any, so the dialog opens with a usable choice already
	// made rather than requiring a click before Ok becomes available.
	for (int row = 0; row < m_targetList->count(); ++row) {
		if (m_targetList->item(row)->flags() & Qt::ItemIsSelectable) {
			m_targetList->setCurrentRow(row);

			return;
		}
	}
}

void ScreenSharePickerDialog::onCurrentRowChanged(int row) {
	QListWidgetItem *item = (row >= 0) ? m_targetList->item(row) : nullptr;
	const bool validSelection = item && (item->flags() & Qt::ItemIsSelectable);

	m_buttonBox->button(QDialogButtonBox::Ok)->setEnabled(validSelection);

	const bool isWindow = validSelection
		&& static_cast< ScreenShareTargetKind >(item->data(KindRole).toInt()) == ScreenShareTargetKind::Window;

	// Disabled outright, not merely left unchecked, when the selection can't actually honour them - see
	// the class comment on why display capture has no per-process audio or cursor compositing to offer.
	m_showCursor->setEnabled(isWindow);
	m_windowAudioOnly->setEnabled(isWindow && m_includeAudio->isChecked());
}

void ScreenSharePickerDialog::onAccepted() {
	QListWidgetItem *item = m_targetList->currentItem();

	if (!item || !(item->flags() & Qt::ItemIsSelectable)) {
		reject();

		return;
	}

	const auto kind  = static_cast< ScreenShareTargetKind >(item->data(KindRole).toInt());
	const int index  = item->data(IndexRole).toInt();

	m_targetKind = kind;

	if (kind == ScreenShareTargetKind::Display && index >= 0 && index < m_displays.size()) {
		m_chosenDisplay = m_displays.at(index);
	} else if (kind == ScreenShareTargetKind::Window && index >= 0 && index < m_windows.size()) {
		m_chosenWindow = m_windows.at(index);
	}

	accept();
}

bool ScreenSharePickerDialog::includeAudio() const {
	return m_includeAudio && m_includeAudio->isChecked();
}

bool ScreenSharePickerDialog::windowAudioOnly() const {
	return m_windowAudioOnly && m_windowAudioOnly->isEnabled() && m_windowAudioOnly->isChecked();
}

bool ScreenSharePickerDialog::showCursor() const {
	return m_showCursor && m_showCursor->isEnabled() && m_showCursor->isChecked();
}
