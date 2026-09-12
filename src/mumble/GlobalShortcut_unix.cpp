// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "GlobalShortcut_unix.h"

#include "Settings.h"
#include "Global.h"

#include <QtCore/QFileSystemWatcher>
#include <QtCore/QSocketNotifier>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#ifndef NO_XINPUT2
#	include <X11/extensions/XI2.h>
#	include <X11/extensions/XInput2.h>
#endif

#ifdef Q_OS_LINUX
#	include <fcntl.h>
#	include <linux/input.h>
#endif

// We have to use a global 'diagnostic ignored' pragmas because
// we still support old versions of GCC. (FreeBSD 9.3 ships with GCC 4.2)
#if defined(__GNUC__)
// ScreenCount(...) and so on are macros that access the private structure and
// cast their return value using old-style-casts. Hence we suppress these warnings
// for this section of code.
#	pragma GCC diagnostic ignored "-Wold-style-cast"
// XKeycodeToKeysym is deprecated.
// For backwards compatibility reasons we want to keep using the
// old function as long as possible. The replacement function
// XkbKeycodeToKeysym requires the XKB extension which isn't
// guaranteed to be present.
#	pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

/**
 * Returns a platform specific GlobalShortcutEngine object.
 *
 * @see GlobalShortcutX
 * @see GlobalShortcutMac
 * @see GlobalShortcutWin
 */
GlobalShortcutEngine *GlobalShortcutEngine::platformInit() {
	return new GlobalShortcutX();
}

GlobalShortcutX::GlobalShortcutX() {
	iXIopcode = -1;
	bRunning  = false;
	m_enabled = true;

	// The XInput2 and polled backends need a dedicated display connection, but
	// evdev does not: it reads /dev/input directly. Opening the display is
	// therefore allowed to fail, and init() decides what is still possible
	// without one. Returning early here instead took evdev - the only backend
	// that works on a pure Wayland session, where there is no display to open -
	// down together with X.
	display = XOpenDisplay(nullptr);

	if (!display) {
		qWarning("GlobalShortcutX: Unable to open dedicated display connection, X-based shortcuts unavailable.");
	}

	init();
}

GlobalShortcutX::~GlobalShortcutX() {
	stop();

	if (display) {
		XCloseDisplay(display);
	}
}

// Closes every /dev/input device this engine opened. Deleting the QFile takes its QSocketNotifier
// with it - the notifier is parented to the file - so this is what actually stops evdev delivering
// key events, as opposed to merely forgetting about the devices.
void GlobalShortcutX::closeInputDevices() {
#ifdef Q_OS_LINUX
	for (QFile *f : qmInputDevices) {
		delete f;
	}

	qmInputDevices.clear();
	qsKeyboards.clear();
#endif
}

void GlobalShortcutX::stop() {
	bRunning = false;
	wait();

	// Evdev devices are part of what init() started, so stopping has to close them too. Leaving them
	// open meant disabling global shortcuts stopped none of the evdev ones: the notifiers stayed live
	// and every keystroke was still read and dispatched, which for a feature whose whole point is
	// reading the keyboard globally is not a cosmetic difference.
	closeInputDevices();

	if (m_watcher) {
		m_watcher->deleteLater();
		m_watcher = nullptr;
	}
	if (m_notifier) {
		m_notifier->deleteLater();
		m_notifier = nullptr;
	}
}

bool GlobalShortcutX::init() {
	// setEnabled() re-enters this, so whatever a previous run left running is torn down first. Without
	// it a re-enable overwrote m_watcher with a second QFileSystemWatcher while the first stayed
	// connected (duplicate directoryChanged deliveries), and called start() on an already running
	// polling thread. A no-op on the constructor's call, where there is nothing to stop yet.
	stop();

#ifdef Q_OS_LINUX
	if (Global::get().s.bEnableEvdev) {
		QString dir = QLatin1String("/dev/input");
		m_watcher   = new QFileSystemWatcher(QStringList(dir), this);
		connect(m_watcher, SIGNAL(directoryChanged(const QString &)), this, SLOT(directoryChanged(const QString &)));
		directoryChanged(dir);

		if (qsKeyboards.isEmpty()) {
			closeInputDevices();

			delete m_watcher;
			m_watcher = nullptr;
			qWarning(
				"GlobalShortcutX: Unable to open any keyboard input devices under /dev/input, falling back to XInput");
		} else {
			// Evdev is up, and that counts as enabled: setEnabled() stores this into m_enabled, so
			// returning false here left enabled() reporting "off" while evdev was busily capturing -
			// and, because the stored state then disagreed with reality, the next enable attempt ran
			// init() a second time over the top of the live one.
			return true;
		}
	}
#endif

	// Everything past this point needs the display connection, so without one
	// there is nothing left to fall back to (ScreenCount() would dereference it).
	if (!display) {
		qWarning("GlobalShortcutX: No display connection and no usable evdev device, global shortcuts are disabled.");
		return false;
	}

	qsRootWindows.clear();
	for (int i = 0; i < ScreenCount(display); ++i)
		qsRootWindows.insert(RootWindow(display, i));

#ifndef NO_XINPUT2
	int evt, error;

	if (Global::get().s.bEnableXInput2 && XQueryExtension(display, "XInputExtension", &iXIopcode, &evt, &error)) {
		int major = XI_2_Major;
		int minor = XI_2_Minor;
		int rc    = XIQueryVersion(display, &major, &minor);
		if (rc != BadRequest) {
			qWarning("GlobalShortcutX: Using XI2 %d.%d", major, minor);

			queryXIMasterList();

			XIEventMask evmask;
			unsigned char mask[(XI_LASTEVENT + 7) / 8];

			memset(&evmask, 0, sizeof(evmask));
			memset(mask, 0, sizeof(mask));

			XISetMask(mask, XI_RawButtonPress);
			XISetMask(mask, XI_RawButtonRelease);
			XISetMask(mask, XI_RawKeyPress);
			XISetMask(mask, XI_RawKeyRelease);
			XISetMask(mask, XI_HierarchyChanged);

			evmask.deviceid = XIAllDevices;
			evmask.mask_len = sizeof(mask);
			evmask.mask     = mask;

			for (Window w : qsRootWindows) {
				XISelectEvents(display, w, &evmask, 1);
			}
			XFlush(display);

			m_notifier = new QSocketNotifier(ConnectionNumber(display), QSocketNotifier::Read, this);
			connect(m_notifier, SIGNAL(activated(int)), this, SLOT(displayReadyRead(int)));

			return true;
		}
	}
#endif
	qWarning("GlobalShortcutX: No XInput support, falling back to polled input. This wastes a lot of CPU resources, so "
			 "please enable one of the other methods.");
	bRunning = true;
	start(QThread::TimeCriticalPriority);

	return true;
}

// Tight loop polling
void GlobalShortcutX::run() {
	Window root = XDefaultRootWindow(display);
	Window root_ret, child_ret;
	int root_x, root_y;
	int win_x, win_y;
	unsigned int mask[2];
	int idx  = 0;
	int next = 0;
	char keys[2][32];

	memset(keys[0], 0, 32);
	memset(keys[1], 0, 32);
	mask[0] = mask[1] = 0;

	while (bRunning) {
		if (bNeedRemap)
			remap();

		msleep(10);

		idx  = next;
		next = idx ^ 1;
		if (XQueryPointer(display, root, &root_ret, &child_ret, &root_x, &root_y, &win_x, &win_y, &mask[next])
			&& XQueryKeymap(display, keys[next])) {
			for (int i = 0; i < 256; ++i) {
				int index     = i / 8;
				int keymask   = 1 << (i % 8);
				bool oldstate = (keys[idx][index] & keymask) != 0;
				bool newstate = (keys[next][index] & keymask) != 0;
				if (oldstate != newstate) {
					handleButton(i, newstate);
				}
			}
			for (int i = 8; i <= 12; ++i) {
				bool oldstate = (mask[idx] & static_cast< unsigned int >(1 << i)) != 0;
				bool newstate = (mask[next] & static_cast< unsigned int >(1 << i)) != 0;
				if (oldstate != newstate) {
					handleButton(0x110 + i, newstate);
				}
			}
		}
	}
}

// Find XI2 master devices so they can be ignored.
void GlobalShortcutX::queryXIMasterList() {
#ifndef NO_XINPUT2
	XIDeviceInfo *info, *dev;
	int ndevices;

	qsMasterDevices.clear();

	dev = info = XIQueryDevice(display, XIAllDevices, &ndevices);
	for (int i = 0; i < ndevices; ++i) {
		switch (dev->use) {
			case XIMasterPointer:
			case XIMasterKeyboard:
				qsMasterDevices.insert(dev->deviceid);
				break;
			default:
				break;
		}

		++dev;
	}
	XIFreeDeviceInfo(info);
#endif
}

bool GlobalShortcutX::canDisable() {
	return true;
}

bool GlobalShortcutX::enabled() {
	return m_enabled;
}

void GlobalShortcutX::setEnabled(bool enabled) {
	// bRunning is only ever true for the polled backend, so testing it here made the "already in this
	// state" check answer differently depending on which backend was in use - re-running init() over a
	// live polling thread, while correctly short-circuiting for evdev and XI2. m_enabled alone is the
	// state, now that init() reports it faithfully.
	if (enabled == m_enabled) {
		return;
	}

	m_enabled = enabled;

	if (!m_enabled) {
		stop();
	} else {
		m_enabled = init();
	}
}

// XInput2 event is ready on socketnotifier.
void GlobalShortcutX::displayReadyRead(int) {
#ifndef NO_XINPUT2
	// The notifier is deleted when shortcuts are disabled, but an activation already queued for it is
	// still delivered afterwards, and acting on that would fire a shortcut the user has switched off.
	if (!m_enabled) {
		return;
	}

	XEvent evt;

	if (bNeedRemap)
		remap();

	while (XPending(display)) {
		XNextEvent(display, &evt);
		XGenericEventCookie *cookie = &evt.xcookie;

		if ((cookie->type != GenericEvent) || (cookie->extension != iXIopcode) || !XGetEventData(display, cookie))
			continue;

		XIDeviceEvent *xide = reinterpret_cast< XIDeviceEvent * >(cookie->data);

		switch (cookie->evtype) {
			case XI_RawKeyPress:
			case XI_RawKeyRelease:
				if (!qsMasterDevices.contains(xide->deviceid))
					handleButton(xide->detail, cookie->evtype == XI_RawKeyPress);
				break;
			case XI_RawButtonPress:
			case XI_RawButtonRelease:
				if (!qsMasterDevices.contains(xide->deviceid))
					handleButton(xide->detail + 0x117, cookie->evtype == XI_RawButtonPress);
				break;
			case XI_HierarchyChanged:
				queryXIMasterList();
		}

		XFreeEventData(display, cookie);
	}
#endif
}

// One of the raw /dev/input devices has ready input
void GlobalShortcutX::inputReadyRead(int) {
#ifdef Q_OS_LINUX
	if (!Global::get().s.bEnableEvdev || !m_enabled) {
		return;
	}

	struct input_event ev;

	if (bNeedRemap)
		remap();

	QFile *f = qobject_cast< QFile * >(sender()->parent());
	if (!f)
		return;

	bool found = false;

	while (f->read(reinterpret_cast< char * >(&ev), sizeof(ev)) == sizeof(ev)) {
		found = true;
		if (ev.type != EV_KEY)
			continue;
		bool down;
		switch (ev.value) {
			case 0:
				down = false;
				break;
			case 1:
				down = true;
				break;
			default:
				continue;
		}
		int evtcode = ev.code + 8;
		handleButton(evtcode, down);
	}

	if (!found) {
		int fd      = f->handle();
		int version = 0;
		if ((ioctl(fd, EVIOCGVERSION, &version) < 0) || (((version >> 16) & 0xFF) < 1)) {
			qWarning("GlobalShortcutX: Removing dead input device %s", qPrintable(f->fileName()));
			qmInputDevices.remove(f->fileName());
			qsKeyboards.remove(f->fileName());
			delete f;
		}
	}
#endif
}

#define test_bit(bit, array) (array[bit / 8] & (1 << (bit % 8)))

// The /dev/input directory changed
void GlobalShortcutX::directoryChanged(const QString &dir) {
#ifdef Q_OS_LINUX
	if (!Global::get().s.bEnableEvdev) {
		return;
	}

	QDir d(dir, QLatin1String("event*"), {}, QDir::System);
	for (const QFileInfo &fi : d.entryInfoList()) {
		QString path = fi.absoluteFilePath();
		if (!qmInputDevices.contains(path)) {
			QFile *f = new QFile(path, this);
			if (f->open(QIODevice::ReadOnly)) {
				int fd = f->handle();
				int version;
				char name[256];
				uint8_t events[EV_MAX / 8 + 1];
				memset(events, 0, sizeof(events));
				if ((ioctl(fd, EVIOCGVERSION, &version) >= 0) && (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0)
					&& (ioctl(fd, EVIOCGBIT(0, sizeof(events)), &events) >= 0) && test_bit(EV_KEY, events)
					&& (((version >> 16) & 0xFF) > 0)) {
					name[255] = 0;
					qWarning("GlobalShortcutX: %s: %s", qPrintable(f->fileName()), name);
					// Is it grabbed by someone else?
					if ((ioctl(fd, EVIOCGRAB, 1) < 0)) {
						qWarning("GlobalShortcutX: Device exclusively grabbed by someone else (X11 using "
								 "exclusive-mode evdev?)");
						delete f;
					} else {
						ioctl(fd, EVIOCGRAB, 0);
						uint8_t keys[KEY_MAX / 8 + 1];
						if ((ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), &keys) >= 0) && test_bit(KEY_SPACE, keys))
							qsKeyboards.insert(f->fileName());

						fcntl(f->handle(), F_SETFL, O_NONBLOCK);
						connect(new QSocketNotifier(f->handle(), QSocketNotifier::Read, f), SIGNAL(activated(int)),
								this, SLOT(inputReadyRead(int)));

						qmInputDevices.insert(f->fileName(), f);
					}
				} else {
					delete f;
				}
			} else {
				delete f;
			}
		}
	}
#else
	Q_UNUSED(dir);
#endif
}

#undef test_bit

GlobalShortcutX::ButtonInfo GlobalShortcutX::buttonInfo(const QVariant &v) {
	bool ok;
	unsigned int key = v.toUInt(&ok);
	if (!ok) {
		return ButtonInfo();
	}

	ButtonInfo info;

	if ((key < 0x118) || (key >= 0x128)) {
		info.device = tr("Keyboard");
		// For backwards compatibility reasons we want to keep using the
		// old function as long as possible. The replacement function
		// XkbKeycodeToKeysym requires the XKB extension which isn't
		// guaranteed to be present.
		// Evdev reports keycodes with no display open, so a keycode can only be
		// named when there is a connection to ask. XKeycodeToKeysym dereferences
		// the display straight away, which made naming a shortcut in the UI a
		// hard crash rather than a missing name.
		KeySym ks = display ? XKeycodeToKeysym(display, static_cast< KeyCode >(key), 0) : NoSymbol;
		if (ks == NoSymbol) {
			info.name = QLatin1String("0x") + QString::number(key, 16);
		} else {
			// Null for a keysym X has no name for, not just the empty string.
			const char *str = XKeysymToString(ks);
			if (!str || str[0] == '\0') {
				info.name = QLatin1String("KS0x") + QString::number(ks, 16);
			} else {
				info.name = QLatin1String(str);
			}
		}
	} else {
		info.device       = tr("Mouse");
		info.devicePrefix = QLatin1String("M");
		info.name         = QString::number(key - 0x118);
	}

	return info;
}
