// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "ScreenSharePickerDialog.h"

#include <QtCore/QMetaObject>
#include <QtCore/QPointer>
#include <QtCore/QRegularExpression>
#include <QtCore/QThreadPool>
#include <QtCore/QTimer>
#include <QtGui/QPainter>
#include <QtGui/QPixmap>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QVBoxLayout>

#include <windows.h>

namespace {

constexpr int IndexRole = Qt::UserRole;

/// PW_RENDERFULLCONTENT: lets PrintWindow capture windows drawn with DirectComposition or the GPU
/// (browsers, most games, UWP apps), which the plain flags leave black. Not in every SDK's headers.
constexpr UINT PRINT_WINDOW_FULL_CONTENT = 0x00000002;

/// A 32-bit top-down DIB section of the given size, selected into a new memory DC.
struct DibCanvas {
	HDC dc        = nullptr;
	HBITMAP bmp   = nullptr;
	HGDIOBJ old   = nullptr;
	void *bits    = nullptr;
	int width     = 0;
	int height    = 0;

	DibCanvas(HDC reference, int w, int h) : width(w), height(h) {
		dc = CreateCompatibleDC(reference);

		BITMAPINFO info{};
		info.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
		info.bmiHeader.biWidth       = w;
		info.bmiHeader.biHeight      = -h;
		info.bmiHeader.biPlanes      = 1;
		info.bmiHeader.biBitCount    = 32;
		info.bmiHeader.biCompression = BI_RGB;

		bmp = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
		if (bmp) {
			old = SelectObject(dc, bmp);
		}
	}

	~DibCanvas() {
		if (old) {
			SelectObject(dc, old);
		}
		if (bmp) {
			DeleteObject(bmp);
		}
		if (dc) {
			DeleteDC(dc);
		}
	}

	bool ok() const { return dc && bmp && bits; }

	QImage toImage() const {
		return QImage(static_cast< const uchar * >(bits), width, height, width * 4, QImage::Format_RGB32).copy();
	}
};

QSize thumbnailSizeFor(int width, int height) {
	return QSize(width, height).scaled(ScreenSharePickerDialog::THUMBNAIL_WIDTH,
										ScreenSharePickerDialog::THUMBNAIL_HEIGHT, Qt::KeepAspectRatio);
}

/// A display, scaled down by GDI while copying, so a 4K desktop is never copied at full size.
QImage grabDisplay(const QRect &geometry) {
	if (geometry.isEmpty()) {
		return QImage();
	}

	const QSize size = thumbnailSizeFor(geometry.width(), geometry.height());
	HDC screen       = GetDC(nullptr);
	QImage image;

	{
		DibCanvas canvas(screen, size.width(), size.height());
		if (canvas.ok()) {
			SetStretchBltMode(canvas.dc, HALFTONE);
			SetBrushOrgEx(canvas.dc, 0, 0, nullptr);
			if (StretchBlt(canvas.dc, 0, 0, size.width(), size.height(), screen, geometry.x(), geometry.y(),
						   geometry.width(), geometry.height(), SRCCOPY | CAPTUREBLT)) {
				image = canvas.toImage();
			}
		}
	}

	ReleaseDC(nullptr, screen);
	return image;
}

/// A window's own drawing, even where other windows cover it. Null for a minimised or hung window -
/// asking a hung window to draw itself would stall this thread for as long as it stays hung.
QImage grabWindow(HWND window) {
	if (!IsWindow(window) || IsIconic(window) || IsHungAppWindow(window)) {
		return QImage();
	}

	RECT rect{};
	if (!GetWindowRect(window, &rect)) {
		return QImage();
	}

	const int width  = rect.right - rect.left;
	const int height = rect.bottom - rect.top;
	if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {
		return QImage();
	}

	HDC screen = GetDC(nullptr);
	QImage image;

	{
		DibCanvas canvas(screen, width, height);
		if (canvas.ok() && PrintWindow(window, canvas.dc, PRINT_WINDOW_FULL_CONTENT)) {
			image = canvas.toImage().scaled(thumbnailSizeFor(width, height), Qt::IgnoreAspectRatio,
											Qt::SmoothTransformation);
		}
	}

	ReleaseDC(nullptr, screen);
	return image;
}

/// The thumbnail centred on a fixed-size dark tile, so every item in the grid lines up.
QIcon tileIcon(const QImage &image, const QString &placeholder) {
	QPixmap tile(ScreenSharePickerDialog::THUMBNAIL_WIDTH, ScreenSharePickerDialog::THUMBNAIL_HEIGHT);
	tile.fill(QColor(32, 32, 32));

	QPainter painter(&tile);
	if (!image.isNull()) {
		const QSize size = image.size().scaled(tile.size(), Qt::KeepAspectRatio);
		painter.drawImage(QRect(QPoint((tile.width() - size.width()) / 2, (tile.height() - size.height()) / 2), size),
						  image);
	} else {
		painter.setPen(QColor(170, 170, 170));
		painter.drawText(tile.rect(), Qt::AlignCenter, placeholder);
	}
	painter.end();

	QIcon icon;
	// The same picture for the selected state: the default would tint the whole thumbnail blue.
	icon.addPixmap(tile, QIcon::Normal);
	icon.addPixmap(tile, QIcon::Selected);
	return icon;
}

QListWidget *makeThumbnailGrid(QWidget *parent) {
	auto *list = new QListWidget(parent);
	list->setViewMode(QListView::IconMode);
	list->setIconSize(QSize(ScreenSharePickerDialog::THUMBNAIL_WIDTH, ScreenSharePickerDialog::THUMBNAIL_HEIGHT));
	list->setGridSize(QSize(ScreenSharePickerDialog::THUMBNAIL_WIDTH + 24, ScreenSharePickerDialog::THUMBNAIL_HEIGHT + 48));
	list->setResizeMode(QListView::Adjust);
	list->setMovement(QListView::Static);
	list->setWrapping(true);
	list->setWordWrap(false);
	list->setTextElideMode(Qt::ElideRight);
	list->setUniformItemSizes(true);
	list->setSpacing(4);
	return list;
}

/// "Screen 2 - 2560 x 1440 (main)": the number Windows' own display settings use, from "\\.\DISPLAY2".
QString displayLabel(const DxgiDisplayVideoSource::DisplayInfo &display, int position) {
	const QRegularExpressionMatch number = QRegularExpression(QStringLiteral("(\\d+)$")).match(display.deviceName);
	const QString name = number.hasMatch() ? ScreenSharePickerDialog::tr("Screen %1").arg(number.captured(1))
										   : ScreenSharePickerDialog::tr("Screen %1").arg(position + 1);
	QString label      = ScreenSharePickerDialog::tr("%1 - %2 x %3")
						.arg(name)
						.arg(display.geometry.width())
						.arg(display.geometry.height());
	if (display.geometry.topLeft() == QPoint(0, 0)) {
		label += ScreenSharePickerDialog::tr(" (main)");
	}
	return label;
}

} // namespace

ScreenSharePickerDialog::ScreenSharePickerDialog(QWidget *parent)
	: QDialog(parent), m_alive(std::make_shared< bool >(true)) {
	setWindowTitle(tr("Share Screen"));
	resize(920, 640);

	m_displays = DxgiDisplayVideoSource::availableDisplays();
	m_windows  = WgcWindowVideoSource::availableWindows();

	auto *layout = new QVBoxLayout(this);

	m_tabs        = new QTabWidget(this);
	m_displayList = makeThumbnailGrid(m_tabs);
	m_windowList  = makeThumbnailGrid(m_tabs);
	m_tabs->addTab(m_displayList, tr("Screens"));
	m_tabs->addTab(m_windowList, tr("Applications"));
	layout->addWidget(m_tabs, 1);

	m_includeAudio = new QCheckBox(tr("Include audio"), this);
	m_includeAudio->setChecked(true);
	layout->addWidget(m_includeAudio);

	m_windowAudioOnly = new QCheckBox(tr("Only this application's audio (not all system sound)"), this);
	// As in Discord: sharing a window shares that window's sound, not whatever else happens to be playing.
	m_windowAudioOnly->setChecked(true);
	layout->addWidget(m_windowAudioOnly);

	m_showCursor = new QCheckBox(tr("Show my cursor"), this);
	m_showCursor->setChecked(true);
	layout->addWidget(m_showCursor);

	m_buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	m_buttonBox->button(QDialogButtonBox::Ok)->setText(tr("Share"));
	layout->addWidget(m_buttonBox);

	populateLists();

	connect(m_tabs, &QTabWidget::currentChanged, this, [this](int) {
		updateControls();
		refreshThumbnails();
	});
	connect(m_displayList, &QListWidget::currentRowChanged, this, &ScreenSharePickerDialog::updateControls);
	connect(m_windowList, &QListWidget::currentRowChanged, this, &ScreenSharePickerDialog::updateControls);
	// Double-clicking a thumbnail shares it, the way it works in Discord.
	connect(m_displayList, &QListWidget::itemActivated, this, &ScreenSharePickerDialog::onAccepted);
	connect(m_windowList, &QListWidget::itemActivated, this, &ScreenSharePickerDialog::onAccepted);

	// windowAudioOnly's availability also depends on whether audio is being included at all.
	connect(m_includeAudio, &QCheckBox::toggled, this, &ScreenSharePickerDialog::updateControls);

	connect(m_buttonBox, &QDialogButtonBox::accepted, this, &ScreenSharePickerDialog::onAccepted);
	connect(m_buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

	m_refreshTimer = new QTimer(this);
	m_refreshTimer->setInterval(THUMBNAIL_REFRESH_MSEC);
	connect(m_refreshTimer, &QTimer::timeout, this, &ScreenSharePickerDialog::refreshThumbnails);
	m_refreshTimer->start();

	updateControls();
	refreshThumbnails();
}

ScreenSharePickerDialog::~ScreenSharePickerDialog() {
	*m_alive = false;
}

void ScreenSharePickerDialog::populateLists() {
	m_displayList->clear();
	m_windowList->clear();

	for (int i = 0; i < m_displays.size(); ++i) {
		auto *item = new QListWidgetItem(tileIcon(QImage(), tr("Loading...")), displayLabel(m_displays.at(i), i),
										 m_displayList);
		item->setData(IndexRole, i);
		item->setToolTip(item->text());
	}

	for (int i = 0; i < m_windows.size(); ++i) {
		auto *item = new QListWidgetItem(tileIcon(QImage(), tr("Loading...")), m_windows.at(i).title, m_windowList);
		item->setData(IndexRole, i);
		item->setToolTip(m_windows.at(i).title);
	}

	// Opens with a usable choice already made, so Share works without a click first.
	if (m_displayList->count() > 0) {
		m_displayList->setCurrentRow(0);
	}
	if (m_windowList->count() > 0) {
		m_windowList->setCurrentRow(0);
	}
	if (m_displayList->count() == 0 && m_windowList->count() > 0) {
		m_tabs->setCurrentWidget(m_windowList);
	}
}

void ScreenSharePickerDialog::refreshThumbnails() {
	if (m_refreshInFlight) {
		return;
	}

	// Only the tab being looked at: no point drawing twenty windows nobody can see.
	const bool windows = m_tabs->currentWidget() == m_windowList;

	std::vector< QRect > displayRects;
	std::vector< HWND > windowHandles;
	if (windows) {
		for (const auto &window : m_windows) {
			windowHandles.push_back(window.handle);
		}
	} else {
		for (const auto &display : m_displays) {
			displayRects.push_back(display.geometry);
		}
	}

	m_refreshInFlight                = true;
	QPointer< ScreenSharePickerDialog > self(this);
	const std::shared_ptr< bool > alive = m_alive;

	QThreadPool::globalInstance()->start([self, alive, windows, displayRects, windowHandles]() {
		std::vector< Thumbnail > thumbnails;

		if (windows) {
			for (std::size_t i = 0; i < windowHandles.size(); ++i) {
				thumbnails.push_back({ true, static_cast< int >(i), grabWindow(windowHandles[i]) });
			}
		} else {
			for (std::size_t i = 0; i < displayRects.size(); ++i) {
				thumbnails.push_back({ false, static_cast< int >(i), grabDisplay(displayRects[i]) });
			}
		}

		// Delivered on the GUI thread, and only if the dialog is still open.
		QMetaObject::invokeMethod(qApp, [self, alive, thumbnails]() {
			if (*alive && self) {
				self->applyThumbnails(thumbnails);
			}
		});
	});
}

void ScreenSharePickerDialog::applyThumbnails(const std::vector< Thumbnail > &thumbnails) {
	m_refreshInFlight = false;

	for (const Thumbnail &thumbnail : thumbnails) {
		QListWidget *list     = thumbnail.isWindow ? m_windowList : m_displayList;
		QListWidgetItem *item = list->item(thumbnail.index);
		if (!item) {
			continue;
		}

		if (!thumbnail.image.isNull()) {
			item->setIcon(tileIcon(thumbnail.image, QString()));
		} else if (thumbnail.isWindow) {
			// A minimised window cannot draw itself. It can still be picked; its picture appears once it is
			// restored.
			item->setIcon(tileIcon(QImage(), tr("No preview (minimized?)")));
		}
	}
}

QListWidgetItem *ScreenSharePickerDialog::currentTargetItem() const {
	const QListWidget *list = m_tabs->currentWidget() == m_windowList ? m_windowList : m_displayList;
	return list->currentItem();
}

void ScreenSharePickerDialog::updateControls() {
	QListWidgetItem *item = currentTargetItem();
	const bool isWindow   = item && m_tabs->currentWidget() == m_windowList;

	m_buttonBox->button(QDialogButtonBox::Ok)->setEnabled(item != nullptr);

	// Disabled outright, not merely left unchecked, when the selection can't actually honour them - see
	// the class comment.
	m_showCursor->setEnabled(isWindow);
	m_windowAudioOnly->setEnabled(isWindow && m_includeAudio->isChecked());
}

void ScreenSharePickerDialog::onAccepted() {
	QListWidgetItem *item = currentTargetItem();

	if (!item) {
		return;
	}

	const int index = item->data(IndexRole).toInt();

	if (m_tabs->currentWidget() == m_windowList) {
		if (index < 0 || index >= m_windows.size()) {
			return;
		}
		m_targetKind   = ScreenShareTargetKind::Window;
		m_chosenWindow = m_windows.at(index);
	} else {
		if (index < 0 || index >= m_displays.size()) {
			return;
		}
		m_targetKind    = ScreenShareTargetKind::Display;
		m_chosenDisplay = m_displays.at(index);
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
