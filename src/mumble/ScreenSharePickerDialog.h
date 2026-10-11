// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_SCREENSHAREPICKERDIALOG_H_
#define MUMBLE_MUMBLE_SCREENSHAREPICKERDIALOG_H_

#include "DxgiDisplayVideoSource.h"
#include "WgcWindowVideoSource.h"

#include <QtCore/QList>
#include <QtGui/QImage>
#include <QtWidgets/QDialog>

#include <memory>
#include <vector>

class QCheckBox;
class QDialogButtonBox;
class QListWidget;
class QListWidgetItem;
class QTabWidget;
class QTimer;

/// What kind of thing a ScreenSharePickerDialog's selection resolved to once accepted.
enum class ScreenShareTargetKind { None, Display, Window };

/**
 * Lets the user choose what to share before a screen share starts: a whole display or a single window,
 * and what audio (if any) and cursor visibility should go with it.
 *
 * Laid out like Discord's: a "Screens" and an "Applications" tab, each a grid of live thumbnails, so
 * "Display 2" is a picture of that display rather than a name to guess at. The thumbnails are taken on a
 * worker thread - a window that is slow to draw itself must not freeze the dialog - and refreshed every
 * THUMBNAIL_REFRESH_MSEC for the tab that is showing. They never leave this computer.
 *
 * "Only this application's audio" and "Show my cursor" only apply to a window: whole-display capture here
 * (DxgiDisplayVideoSource) has no single application to scope audio to and does not composite a cursor
 * into its frames. Both are disabled, not merely unchecked, while a display is selected. Sharing a window
 * shares only that window's sound by default, as Discord does.
 */
class ScreenSharePickerDialog : public QDialog {
	Q_OBJECT

public:
	explicit ScreenSharePickerDialog(QWidget *parent = nullptr);
	~ScreenSharePickerDialog() override;

	static constexpr int THUMBNAIL_WIDTH          = 256;
	static constexpr int THUMBNAIL_HEIGHT         = 144;
	static constexpr int THUMBNAIL_REFRESH_MSEC   = 2000;

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
	void updateControls();
	void refreshThumbnails();

protected:
	struct Thumbnail {
		bool isWindow = false;
		int index     = 0;
		QImage image;
	};

	void populateLists();
	QListWidgetItem *currentTargetItem() const;
	void applyThumbnails(const std::vector< Thumbnail > &thumbnails);

	QTabWidget *m_tabs             = nullptr;
	QListWidget *m_displayList     = nullptr;
	QListWidget *m_windowList      = nullptr;
	QCheckBox *m_includeAudio      = nullptr;
	QCheckBox *m_windowAudioOnly   = nullptr;
	QCheckBox *m_showCursor        = nullptr;
	QDialogButtonBox *m_buttonBox  = nullptr;
	QTimer *m_refreshTimer         = nullptr;
	/// A refresh still running on the worker thread: the next one waits for it rather than piling up.
	bool m_refreshInFlight = false;
	/// Shared with the worker's completion callback, which may arrive after this dialog is gone.
	std::shared_ptr< bool > m_alive;

	QList< DxgiDisplayVideoSource::DisplayInfo > m_displays;
	QList< WgcWindowVideoSource::WindowInfo > m_windows;

	ScreenShareTargetKind m_targetKind = ScreenShareTargetKind::None;
	DxgiDisplayVideoSource::DisplayInfo m_chosenDisplay;
	WgcWindowVideoSource::WindowInfo m_chosenWindow;
};

#endif // MUMBLE_MUMBLE_SCREENSHAREPICKERDIALOG_H_
