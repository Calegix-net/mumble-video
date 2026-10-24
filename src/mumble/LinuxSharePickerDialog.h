// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_LINUXSHAREPICKERDIALOG_H_
#define MUMBLE_MUMBLE_LINUXSHAREPICKERDIALOG_H_

#include "PortalScreenCast.h"

#include <QtCore/QList>
#include <QtCore/QPointer>
#include <QtGui/QScreen>
#include <QtWidgets/QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QRadioButton;
class QToolButton;

/**
 * The Linux counterpart of the Windows ScreenSharePickerDialog: the same choices - a screen or a window,
 * whether to include audio and from which application, whether to show the cursor - asked the same way.
 *
 * What it cannot do is show the screens and windows themselves. Under Wayland an application may not list
 * or look at other windows; only the desktop's own picker (the screen-cast portal) can, and it already shows
 * previews. So this dialog settles everything else, and the portal's picker follows it for the one choice
 * only it can offer. The portal is asked for just a screen or just a window, as chosen here.
 *
 * The portal does not say which program owns the window that was picked, so "only this application's
 * audio" cannot follow the window as it does on Windows. Instead the applications playing sound right now
 * are listed and one can be chosen.
 *
 * With directCapture, the share is going to X11ScreenVideoSource instead, because no screen-cast portal
 * answered. There is then no desktop picker to follow, so this dialog asks which screen itself when there
 * is more than one. A window cannot be chosen and the cursor cannot be shown: that source grabs a region
 * of the root window, which holds neither a window of its own nor the cursor.
 *
 * The choices are remembered for the rest of the session.
 */
class LinuxSharePickerDialog : public QDialog {
	Q_OBJECT

public:
	explicit LinuxSharePickerDialog(QWidget *parent = nullptr, bool directCapture = false);

	PortalScreenCast::SourceType sourceType() const;
	/// The screen to capture with directCapture; nullptr without it, or if that screen has gone since.
	QScreen *captureScreen() const;

	/// Whether only part of that screen should be shared, chosen by dragging a box over it once this
	/// dialog closes. Only ever true with directCapture - see the constructor for why the portal cannot
	/// offer this yet.
	bool regionRequested() const;
	bool includeAudio() const;
	/// The application whose sound to share (a PulseLoopbackSource::Application key); empty for every
	/// application except Mumble. Meaningful only when includeAudio().
	QString audioApplicationKey() const;
	QString audioApplicationName() const;
	bool showCursor() const;

protected slots:
	void refreshApplications();
	void updateControls();
	void remember();

protected:
	bool m_directCapture = false;
	QList< QPointer< QScreen > > m_screens;
	QComboBox *m_screenChoice = nullptr;

	QRadioButton *m_screen          = nullptr;
	QRadioButton *m_region          = nullptr;
	QRadioButton *m_window          = nullptr;
	QCheckBox *m_includeAudio       = nullptr;
	QLabel *m_audioFromLabel        = nullptr;
	QComboBox *m_audioFrom          = nullptr;
	QToolButton *m_refreshAudioFrom = nullptr;
	QCheckBox *m_showCursor         = nullptr;
};

#endif // MUMBLE_MUMBLE_LINUXSHAREPICKERDIALOG_H_
