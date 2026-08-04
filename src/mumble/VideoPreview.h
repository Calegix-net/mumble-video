// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_VIDEOPREVIEW_H_
#define MUMBLE_MUMBLE_VIDEOPREVIEW_H_

#include <QtGui/QImage>
#include <QtWidgets/QWidget>

/**
 * Shows a single video frame, letterboxed.
 *
 * Kept in its own header rather than beside the settings page it was first written for: the wizard uses
 * it too, and reaching it through VideoConfigDialog.h would drag the whole client configuration
 * machinery -- and its generated UI headers -- along with it.
 */
class VideoPreview : public QWidget {
	Q_OBJECT

public:
	explicit VideoPreview(QWidget *parent = nullptr);

	void setFrame(const QImage &frame);
	void clear();

protected:
	QImage m_frame;

	void paintEvent(QPaintEvent *event) override;
	QSize sizeHint() const override { return QSize(320, 240); }
};

#endif // MUMBLE_MUMBLE_VIDEOPREVIEW_H_
