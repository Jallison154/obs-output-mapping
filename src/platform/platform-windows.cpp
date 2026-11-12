/*
Output Mapping for OBS Studio
Copyright (C) 2026 Okami Designs

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Windows display identity and output placement.
//
// Identity
//   uuid   = DisplayConfig monitorDevicePath, e.g.
//            \\?\DISPLAY#DEL41A3#5&1c2f&0&UID4353#{e6f07b5f-...}
//            Stable for a given monitor on a given GPU connector, survives
//            reboots, resolution changes, rearranging and primary changes.
//   vendor / model = EDID manufacturer / product code
//   serial = EDID serial number (numeric, or a hash of the 0xFF serial
//            string descriptor when the numeric field is 0)
//   Moving a monitor to another connector changes the path; it is then
//   re-identified only by a unique non-zero serial (see display-match.hpp).
//
// Placement
//   Outputs are WS_POPUP, topmost, tool windows (no taskbar button) that
//   never activate, positioned with SetWindowPos at the monitor's exact
//   physical rect so the swapchain is the monitor's native pixel size
//   regardless of Windows display scaling (OBS is per-monitor DPI aware).

/* Windows headers first, targeting Windows 10 so SetWindowDisplayAffinity,
 * DisplayConfig and per-monitor DPI APIs are declared. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0A00
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <setupapi.h>
#include <initguid.h>
#include <devguid.h>

#include "platform/platform.hpp"

#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QGuiApplication>
#include <QScreen>
#include <QWidget>
#include <QtGui/qscreen_platform.h>

#include <memory>
#include <string>

namespace om {

/* ---------------------------------------------------------------------- */
/* Helpers                                                                 */

static QString fromWide(const wchar_t *s)
{
	return QString::fromWCharArray(s);
}

static HMONITOR hmonitorFor(QScreen *s)
{
	if (!s)
		return nullptr;
	if (auto *ws = s->nativeInterface<QNativeInterface::QWindowsScreen>())
		return ws->handle();
	return nullptr;
}

static bool monitorInfo(HMONITOR m, MONITORINFOEXW &mi)
{
	if (!m)
		return false;
	mi = {};
	mi.cbSize = sizeof(mi);
	return GetMonitorInfoW(m, reinterpret_cast<MONITORINFO *>(&mi)) != 0;
}

/* "\\?\DISPLAY#DEL41A3#5&1c2f&0&UID4353#{guid}" -> "DISPLAY\DEL41A3\5&1c2f&0&UID4353" */
static std::wstring instanceIdFromDevicePath(const std::wstring &path)
{
	std::wstring p = path;
	if (p.rfind(L"\\\\?\\", 0) == 0)
		p = p.substr(4);
	size_t guid = p.rfind(L"#{");
	if (guid != std::wstring::npos)
		p = p.substr(0, guid);
	for (auto &ch : p)
		if (ch == L'#')
			ch = L'\\';
	return p;
}

struct Edid {
	bool valid = false;
	uint32_t vendor = 0, product = 0, serial = 0;
	QString name;
};

static Edid readEdid(const std::wstring &devicePath)
{
	Edid out;
	std::wstring inst = instanceIdFromDevicePath(devicePath);
	HDEVINFO set = SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_MONITOR, nullptr);
	if (set == INVALID_HANDLE_VALUE)
		return out;
	SP_DEVINFO_DATA dev = {};
	dev.cbSize = sizeof(dev);
	if (SetupDiOpenDeviceInfoW(set, inst.c_str(), nullptr, 0, &dev)) {
		HKEY key = SetupDiOpenDevRegKey(set, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
		if (key != INVALID_HANDLE_VALUE) {
			BYTE buf[1024];
			DWORD size = sizeof(buf), type = 0;
			if (RegQueryValueExW(key, L"EDID", nullptr, &type, buf, &size) == ERROR_SUCCESS && size >= 128 &&
			    buf[0] == 0x00 && buf[1] == 0xFF && buf[7] == 0x00) {
				out.valid = true;
				out.vendor = (uint32_t)((buf[8] << 8) | buf[9]);
				out.product = (uint32_t)(buf[10] | (buf[11] << 8));
				out.serial = (uint32_t)(buf[12] | (buf[13] << 8) | (buf[14] << 16) | ((uint32_t)buf[15] << 24));
				/* Detailed descriptors: 0xFF serial string, 0xFC name. */
				QByteArray serialText;
				for (int d = 54; d + 18 <= 126; d += 18) {
					if (buf[d] != 0 || buf[d + 1] != 0)
						continue;
					QByteArray text = QByteArray(reinterpret_cast<const char *>(buf + d + 5), 13);
					int nl = text.indexOf('\n');
					if (nl >= 0)
						text.truncate(nl);
					text = text.trimmed();
					if (buf[d + 3] == 0xFF)
						serialText = text;
					else if (buf[d + 3] == 0xFC)
						out.name = QString::fromLatin1(text);
				}
				if (out.serial == 0 && !serialText.isEmpty()) {
					QByteArray h = QCryptographicHash::hash(serialText, QCryptographicHash::Sha1);
					out.serial = (uint32_t)((uint8_t)h[0] << 24 | (uint8_t)h[1] << 16 | (uint8_t)h[2] << 8 |
								(uint8_t)h[3]);
					if (out.serial == 0)
						out.serial = 1;
				}
			}
			RegCloseKey(key);
		}
	}
	SetupDiDestroyDeviceInfoList(set);
	return out;
}

static bool isInternal(DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY t)
{
	return t == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL || t == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED ||
	       t == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED || t == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_LVDS;
}

/* ---------------------------------------------------------------------- */
/* Enumeration                                                             */

std::vector<ScreenInfo> enumerateScreens()
{
	std::vector<ScreenInfo> out;

	UINT32 numPath = 0, numMode = 0;
	std::vector<DISPLAYCONFIG_PATH_INFO> paths;
	std::vector<DISPLAYCONFIG_MODE_INFO> modes;
	LONG rc = ERROR_INSUFFICIENT_BUFFER;
	for (int attempt = 0; attempt < 4 && rc == ERROR_INSUFFICIENT_BUFFER; attempt++) {
		if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &numPath, &numMode) != ERROR_SUCCESS)
			return out;
		paths.resize(numPath);
		modes.resize(numMode);
		rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &numPath, paths.data(), &numMode, modes.data(), nullptr);
	}
	if (rc != ERROR_SUCCESS)
		return out;
	paths.resize(numPath);

	/* QScreens by GDI device name. */
	const QList<QScreen *> qscreens = QGuiApplication::screens();
	auto qscreenFor = [&](const QString &gdi) -> QScreen * {
		for (QScreen *qs : qscreens) {
			MONITORINFOEXW mi;
			if (monitorInfo(hmonitorFor(qs), mi) && fromWide(mi.szDevice).compare(gdi, Qt::CaseInsensitive) == 0)
				return qs;
		}
		for (QScreen *qs : qscreens)
			if (qs->name().compare(gdi, Qt::CaseInsensitive) == 0)
				return qs;
		return nullptr;
	};

	for (const DISPLAYCONFIG_PATH_INFO &path : paths) {
		DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
		source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
		source.header.size = sizeof(source);
		source.header.adapterId = path.sourceInfo.adapterId;
		source.header.id = path.sourceInfo.id;
		if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS)
			continue;
		const QString gdi = fromWide(source.viewGdiDeviceName);

		/* Clone/duplicate mode: several targets share one source. Report
		 * the source once and flag it. */
		bool duplicate = false;
		for (ScreenInfo &si : out) {
			if (si.nativeId.compare(gdi, Qt::CaseInsensitive) == 0) {
				si.mirrored = true;
				duplicate = true;
			}
		}
		if (duplicate)
			continue;

		DISPLAYCONFIG_TARGET_DEVICE_NAME target = {};
		target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
		target.header.size = sizeof(target);
		target.header.adapterId = path.targetInfo.adapterId;
		target.header.id = path.targetInfo.id;
		bool haveTarget = DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS;

		ScreenInfo si;
		si.nativeId = gdi;
		if (path.sourceInfo.modeInfoIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID &&
		    path.sourceInfo.modeInfoIdx < modes.size() &&
		    modes[path.sourceInfo.modeInfoIdx].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
			const DISPLAYCONFIG_SOURCE_MODE &sm = modes[path.sourceInfo.modeInfoIdx].sourceMode;
			si.pixelW = (int)sm.width;
			si.pixelH = (int)sm.height;
			si.primary = sm.position.x == 0 && sm.position.y == 0;
		}
		/* Outputs are placed at the monitor's physical rect, so the swapchain
		 * always matches the desktop mode. (GPU scaling to the panel is done
		 * after us and is reported by the mode, not by Windows scaling.) */
		si.backingW = si.pixelW;
		si.backingH = si.pixelH;
		si.builtin = isInternal(path.targetInfo.outputTechnology);

		if (haveTarget) {
			std::wstring devicePath = target.monitorDevicePath;
			si.identity.uuid = QString::fromWCharArray(target.monitorDevicePath).toLower().toStdString();
			si.name = fromWide(target.monitorFriendlyDeviceName);
			if (target.flags.edidIdsValid) {
				/* DisplayConfig reports the manufacturer id byte-swapped. */
				uint16_t m = target.edidManufactureId;
				si.identity.vendor = (uint32_t)(((m & 0xFF) << 8) | (m >> 8));
				si.identity.model = target.edidProductCodeId;
			}
			Edid e = readEdid(devicePath);
			if (e.valid) {
				si.identity.vendor = e.vendor;
				si.identity.model = e.product;
				si.identity.serial = e.serial;
				if (si.name.isEmpty())
					si.name = e.name;
			}
		}
		/* No device path (some virtual/indirect display drivers): GDI
		 * names like \\.\DISPLAY3 are reassigned by Windows, so they must
		 * never count as an identity. Such a display is only ever matched
		 * as Ambiguous (by name) and needs confirming each session. */
		if (si.name.isEmpty())
			si.name = si.builtin ? QStringLiteral("Built-in Display") : gdi.mid(gdi.lastIndexOf('\\') + 1);
		si.identity.name = si.name.toStdString();

		si.screen = qscreenFor(gdi);
		if (si.screen) {
			si.geometry = si.screen->geometry();
			si.dpr = si.screen->devicePixelRatio();
		}
		out.push_back(si);
	}

	/* Same friendly name twice (identical monitors): append the GDI index so
	 * the user can tell them apart in menus. */
	for (size_t i = 0; i < out.size(); i++)
		for (size_t j = i + 1; j < out.size(); j++)
			if (out[i].name == out[j].name) {
				out[j].name += QStringLiteral(" (%1)").arg(out[j].nativeId.mid(out[j].nativeId.lastIndexOf('\\') + 1));
				if (!out[i].name.contains('('))
					out[i].name += QStringLiteral(" (%1)").arg(out[i].nativeId.mid(out[i].nativeId.lastIndexOf('\\') + 1));
			}
	return out;
}

/* ---------------------------------------------------------------------- */
/* Output windows                                                          */

void configureOutputWindow(QWidget *w, QScreen *screen, const OutputWindowOptions &opts)
{
	HWND hwnd = reinterpret_cast<HWND>(w->winId());
	MONITORINFOEXW mi;
	if (!hwnd || !monitorInfo(hmonitorFor(screen), mi))
		return;

	LONG_PTR style = WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
	SetWindowLongPtrW(hwnd, GWL_STYLE, style);
	LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
	ex &= ~(LONG_PTR)WS_EX_APPWINDOW;
	ex |= WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
	if (opts.allowFocus)
		ex &= ~(LONG_PTR)WS_EX_NOACTIVATE;
	else
		ex |= WS_EX_NOACTIVATE;
	SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);

	const RECT &r = mi.rcMonitor;
	SetWindowPos(hwnd, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
		     SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);

	/* Keep outputs out of Display/Window Capture so Program can never
	 * contain its own output (feedback). WDA_EXCLUDEFROMCAPTURE needs
	 * Windows 10 2004+; older systems fall back to a black capture. */
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
	if (opts.excludeFromCapture) {
		if (!SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE))
			SetWindowDisplayAffinity(hwnd, WDA_MONITOR);
	} else {
		SetWindowDisplayAffinity(hwnd, WDA_NONE);
	}
}

bool outputWindowOnTarget(QWidget *w, QScreen *screen)
{
	if (!screen || !QGuiApplication::screens().contains(screen))
		return false;
	HWND hwnd = reinterpret_cast<HWND>(w->winId());
	MONITORINFOEXW want, have;
	if (!monitorInfo(hmonitorFor(screen), want))
		return false;
	HMONITOR cur = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
	if (!monitorInfo(cur, have) || wcscmp(want.szDevice, have.szDevice) != 0)
		return false;
	RECT wr;
	if (!GetWindowRect(hwnd, &wr))
		return false;
	return EqualRect(&wr, &want.rcMonitor) != 0;
}

QSize nativePixelSize(QWidget *w)
{
	RECT rc;
	if (GetClientRect(reinterpret_cast<HWND>(w->winId()), &rc))
		return QSize(rc.right - rc.left, rc.bottom - rc.top);
	return w->size() * w->devicePixelRatioF();
}

/* ---------------------------------------------------------------------- */
/* WM_DISPLAYCHANGE hook                                                   */

class DisplayChangeFilter : public QAbstractNativeEventFilter {
public:
	explicit DisplayChangeFilter(std::function<void()> fn) : fn_(std::move(fn)) {}
	bool nativeEventFilter(const QByteArray &type, void *message, qintptr *) override
	{
		if (type != "windows_generic_MSG" || !message)
			return false;
		const MSG *msg = static_cast<const MSG *>(message);
		if (msg->message == WM_DISPLAYCHANGE ||
		    (msg->message == WM_DEVICECHANGE && msg->wParam == 0x0007 /* DBT_DEVNODES_CHANGED */))
			fn_();
		return false;
	}

private:
	std::function<void()> fn_;
};

static std::unique_ptr<DisplayChangeFilter> g_filter;

void installDisplayChangeHook(std::function<void()> fn)
{
	removeDisplayChangeHook();
	g_filter = std::make_unique<DisplayChangeFilter>(std::move(fn));
	QCoreApplication::instance()->installNativeEventFilter(g_filter.get());
}

void removeDisplayChangeHook()
{
	if (g_filter && QCoreApplication::instance())
		QCoreApplication::instance()->removeNativeEventFilter(g_filter.get());
	g_filter.reset();
}

/* ---------------------------------------------------------------------- */

void setDisplaySleepInhibited(bool inhibit)
{
	SetThreadExecutionState(inhibit ? (ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED) : ES_CONTINUOUS);
}

QString modKeyName()
{
	return QStringLiteral("Ctrl");
}

QString altKeyName()
{
	return QStringLiteral("Alt");
}

} // namespace om
