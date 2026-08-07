// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_SCREENSHAREPICKERDIALOG_H_
#define MUMBLE_MUMBLE_SCREENSHAREPICKERDIALOG_H_

#include "DxgiDisplayVideoSource.h"
#include "WgcWindowVideoSource.h"

#include <QtCore/QList>
#include <QtWidgets/QDialog>

class QCheckBox;
class QDialogButtonBox;
class QListWidget;
class QListWidgetItem;

/// What kind of thing a ScreenSharePickerDialog's selection resolved to once accepted.
enum class ScreenShareTargetKind { None, Display, Window };

/**
 * Lets the user choose what to share before a screen share starts: a whole display or a single window,
 * and what audio (if any) and cursor visibility should go with it.
 *
 * Deliberately not folded into VideoWizard: the wizard is a one-time guided bandwidth-measurement flow
 * run once from Configure menu, hardcoded to a camera. This is a per-share, every-time chooser - the same
 * relationship Discord's screen-share button has to its own, separate settings.
 *
 * Displays and windows this build can enumerate are listed directly, in one combined list under two
 * section headers; there is no live preview, unlike Discord's thumbnails, since that would mean running a
 * low-rate capture per candidate just to populate a list the user may not even open - a reasonable
 * improvement for later, not required for this to work.
 *
 * "Only this application's audio" and "Show my cursor" only apply to a window selection: whole-display
 * capture here (DxgiDisplayVideoSource) has no per-process audio concept to scope to - system audio is
 * the whole point - and does not composite a cursor into its frames, so offering that checkbox for a
 * display would promise something that is not actually wired up. Both are disabled, not merely unchecked,
 * when a display is selected, so the dialog does not silently ignore a choice the user thinks they made.
 */
class ScreenSharePickerDialog : public QDialog {
	Q_OBJECT

public:
	explicit ScreenSharePickerDialog(QWidget *parent = nullptr);

	/// None if the dialog was cancelled, or if there was nothing to pick at all. Meaningful only after
	/// exec() has returned QDialog::Accepted.
	ScreenShareTargetKind targetKind() const { return m_targetKind; }

	/// Populated only when targetKind() == Display.
	DxgiDisplayVideoSource::DisplayInfo chosenDisplay() const { return m_chosenDisplay; }

	/// Populated only when targetKind() == Window.
	WgcWindowVideoSource::WindowInfo chosenWindow() const { return m_chosenWindow; }

	/// Whether the "include audio" box was checked. Meaningless if the dialog was cancelled.
	bool includeAudio() const;

	/// Whether audio should be scoped to just the shared window's owning process, rather than all system
	/// audio. Only meaningful when targetKind() == Window and includeAudio() is true.
	bool windowAudioOnly() const;

	/// Whether the shared window should show the sharer's mouse cursor. Only meaningful when
	/// targetKind() == Window.
	bool showCursor() const;

protected slots:
	void onAccepted();
	void onCurrentRowChanged(int row);

protected:
	void populateList();

	QListWidget *m_targetList      = nullptr;
	QCheckBox *m_includeAudio      = nullptr;
	QCheckBox *m_windowAudioOnly   = nullptr;
	QCheckBox *m_showCursor        = nullptr;
	QDialogButtonBox *m_buttonBox  = nullptr;

	QList< DxgiDisplayVideoSource::DisplayInfo > m_displays;
	QList< WgcWindowVideoSource::WindowInfo > m_windows;

	ScreenShareTargetKind m_targetKind = ScreenShareTargetKind::None;
	DxgiDisplayVideoSource::DisplayInfo m_chosenDisplay;
	WgcWindowVideoSource::WindowInfo m_chosenWindow;
};

#endif // MUMBLE_MUMBLE_SCREENSHAREPICKERDIALOG_H_
