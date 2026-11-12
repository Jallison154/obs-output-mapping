/*
Output Mapping for OBS Studio
Copyright (C) 2026 Okami Designs

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// OS-specific display identity and output-window handling.
//   platform-windows.cpp : DisplayConfig + EDID identity, native HWND placement
//   platform-macos.mm    : CoreGraphics UUID + EDID identity, NSWindow placement
//   platform-qt.cpp      : generic QScreen fallback (Linux)

#pragma once

#include "core/display-match.hpp"

#include <QRect>
#include <QSize>
#include <QString>

#include <functional>
#include <vector>

class QScreen;
class QWidget;

namespace om {

/* A display connected right now, as seen by the OS. */
struct ScreenInfo {
	DisplayIdentity identity;
	QString name;           /* e.g. "DELL U2720Q" */
	QString nativeId;       /* Windows: \\.\DISPLAYn   macOS: CGDirectDisplayID */
	int pixelW = 0;         /* current mode in physical pixels (= the HDMI signal) */
	int pixelH = 0;
	int backingW = 0;       /* swapchain size a fullscreen window will get */
	int backingH = 0;
	QRect geometry;         /* Qt desktop coordinates (logical) */
	double dpr = 1.0;
	bool builtin = false;
	bool primary = false;
	bool mirrored = false;  /* duplicated/cloned to other displays by the OS */
	QScreen *screen = nullptr;

	/* False when the OS resamples (macOS "looks like" modes): 1:1 is then
	 * impossible on this display. */
	bool exactPixels() const { return backingW == pixelW && backingH == pixelH; }
};

std::vector<ScreenInfo> enumerateScreens();

struct OutputWindowOptions {
	bool excludeFromCapture = true; /* hide from screen/display capture (no feedback loops) */
	bool allowFocus = false;        /* only when the output covers the OBS window's screen */
};

/* Turn a frameless top-level widget into a borderless output covering
 * `screen` exactly: above taskbar/menu bar, no shadow, never activating. */
void configureOutputWindow(QWidget *w, QScreen *screen, const OutputWindowOptions &opts);

/* True while `w` is still on `screen` and exactly covers it. Checked by a
 * watchdog so an output never lingers on the wrong display. */
bool outputWindowOnTarget(QWidget *w, QScreen *screen);

/* Exact swapchain size for a native widget in physical pixels. */
QSize nativePixelSize(QWidget *w);

/* Call `fn` synchronously when the OS reports a display topology change
 * (Windows: WM_DISPLAYCHANGE, before Qt has rearranged windows). */
void installDisplayChangeHook(std::function<void()> fn);
void removeDisplayChangeHook();

/* Keep displays awake while any fullscreen output is running. */
void setDisplaySleepInhibited(bool inhibit);

/* Human-readable modifier names for hints ("Ctrl"/"⌘", "Alt"/"⌥"). */
QString modKeyName();
QString altKeyName();

} // namespace om
