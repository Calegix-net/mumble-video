// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_DXGIDISPLAYVIDEOSOURCE_H_
#define MUMBLE_MUMBLE_DXGIDISPLAYVIDEOSOURCE_H_

#include "Timer.h"
#include "VideoSource.h"
#include "win.h"

#include <QtCore/QList>
#include <QtCore/QRect>
#include <QtCore/QString>
#include <QtCore/QTimer>

#include <d3d11.h>
#include <dxgi1_2.h>

/**
 * Captures a whole display via DXGI Desktop Duplication.
 *
 * Chosen over a GDI screen-scrape (BitBlt of the desktop DC) because duplication only copies what
 * changed since the last frame and hands back a GPU texture: a scrape would burn a full-resolution CPU
 * copy on every poll regardless of how static the desktop is, which is exactly the case TiledImageEncoder
 * downstream exists to exploit. It is also the only one of the two that reliably captures GPU-composited
 * content at all on a modern desktop.
 *
 * Duplication is poll-based, not push: AcquireNextFrame blocks up to a timeout and returns "nothing
 * changed" as a normal, frequent outcome rather than an error, which is why this drives itself from a
 * QTimer instead of waiting on a capture callback the way CameraVideoSource does.
 *
 * A duplication handle is tied to one process's D3D11 device and does not survive everything that can
 * happen to a display while it runs: a resolution change, a fullscreen-exclusive application taking the
 * output, or the output being disconnected all invalidate it. Recovering means tearing the whole duplication
 * down and re-acquiring it, which this does transparently; only an output that stops existing at all
 * reaches failed().
 */
class DxgiDisplayVideoSource : public VideoSource {
	Q_OBJECT

public:
	struct DisplayInfo {
		/// DXGI_OUTPUT_DESC::DeviceName, e.g. "\\.\DISPLAY1". Not for display to the user - it identifies
		/// which physical output to duplicate, and is re-resolved at start() rather than cached as an
		/// index, since adapters/outputs can be added or removed between enumeration and use.
		QString deviceName;

		/// What the picker shows. The same as deviceName for now: a friendlier label (monitor make/model)
		/// would need EnumDisplayDevices or the newer DisplayConfig API layered on top, which is worth
		/// doing but is not required for capture to work.
		QString friendlyName;

		/// Desktop coordinates, for reference only - the capture itself always covers the whole output.
		QRect geometry;
	};

	/**
	 * Enumerates the displays this machine can duplicate. May be empty: no adapter exposing DXGI 1.2
	 * (duplication needs Windows 8 or newer), or a headless machine.
	 */
	static QList< DisplayInfo > availableDisplays();

	explicit DxgiDisplayVideoSource(const DisplayInfo &display, QObject *parent = nullptr);
	~DxgiDisplayVideoSource() override;

	bool start() override;
	void stop() override;
	bool isRunning() const override;
	QString describe() const override;

protected slots:
	void pollFrame();

protected:
	/// Builds the D3D11 device and duplication handle for m_display. Returns false, having emitted
	/// failed(), if the output cannot be found or duplicated at all - as opposed to AcquireNextFrame
	/// failing transiently later, which is recoverable.
	bool acquireDuplication();

	/// Releases every COM interface this holds, in reverse of acquisition order, and resets the pointers
	/// to null so isRunning() and subsequent calls see a clean slate.
	void releaseDuplication();

	DisplayInfo m_display;

	ID3D11Device *m_device               = nullptr;
	ID3D11DeviceContext *m_context       = nullptr;
	IDXGIOutputDuplication *m_duplication = nullptr;

	// Re-created only when the frame size changes, which for a single output is only ever on the first
	// frame - but a resolution change forces a fresh duplication (see acquireDuplication), and with it a
	// fresh staging texture.
	ID3D11Texture2D *m_stagingTexture = nullptr;
	UINT m_stagingWidth               = 0;
	UINT m_stagingHeight              = 0;

	QTimer m_pollTimer;
	Timer m_clock;

	bool m_running = false;
};

#endif // MUMBLE_MUMBLE_DXGIDISPLAYVIDEOSOURCE_H_
