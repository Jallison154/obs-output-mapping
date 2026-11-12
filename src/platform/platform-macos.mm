/*
Output Mapping for OBS Studio
Copyright (C) 2026 Okami Designs

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "platform/platform.hpp"

#include <QGuiApplication>
#include <QScreen>
#include <QWidget>

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <IOKit/pwr_mgt/IOPMLib.h>

namespace om {

static QString cfUuidString(CGDirectDisplayID did)
{
	CFUUIDRef uuid = CGDisplayCreateUUIDFromDisplayID(did);
	if (!uuid)
		return QString();
	CFStringRef s = CFUUIDCreateString(kCFAllocatorDefault, uuid);
	QString out = QString::fromCFString(s);
	CFRelease(s);
	CFRelease(uuid);
	return out;
}

static NSScreen *nsScreenFor(CGDirectDisplayID did)
{
	for (NSScreen *s in [NSScreen screens]) {
		NSNumber *n = s.deviceDescription[@"NSScreenNumber"];
		if (n && (CGDirectDisplayID)n.unsignedIntValue == did)
			return s;
	}
	return nil;
}

std::vector<ScreenInfo> enumerateScreens()
{
	std::vector<ScreenInfo> out;
	uint32_t count = 0;
	CGDirectDisplayID ids[32];
	if (CGGetActiveDisplayList(32, ids, &count) != kCGErrorSuccess)
		return out;

	const QList<QScreen *> qscreens = QGuiApplication::screens();

	for (uint32_t i = 0; i < count; i++) {
		CGDirectDisplayID did = ids[i];
		/* Skip hardware mirror slaves: they show the master's content. */
		if (CGDisplayIsInMirrorSet(did) && CGDisplayMirrorsDisplay(did) != kCGNullDirectDisplay)
			continue;

		ScreenInfo si;
		si.nativeId = QString::number(did);
		si.identity.uuid = cfUuidString(did).toStdString();
		si.identity.vendor = CGDisplayVendorNumber(did);
		si.identity.model = CGDisplayModelNumber(did);
		si.identity.serial = CGDisplaySerialNumber(did);
		si.builtin = CGDisplayIsBuiltin(did);
		si.primary = CGDisplayIsMain(did);

		CGDisplayModeRef mode = CGDisplayCopyDisplayMode(did);
		if (mode) {
			si.pixelW = (int)CGDisplayModeGetPixelWidth(mode);
			si.pixelH = (int)CGDisplayModeGetPixelHeight(mode);
			CGDisplayModeRelease(mode);
		}

		CGRect b = CGDisplayBounds(did); /* global points, top-left origin like Qt */
		si.geometry = QRect((int)b.origin.x, (int)b.origin.y, (int)b.size.width, (int)b.size.height);

		NSScreen *ns = nsScreenFor(did);
		if (ns) {
			si.dpr = ns.backingScaleFactor;
			if (@available(macOS 10.15, *))
				si.name = QString::fromNSString(ns.localizedName);
		}
		if (si.name.isEmpty())
			si.name = si.builtin ? QStringLiteral("Built-in Display") : QStringLiteral("Display %1").arg(did);
		si.identity.name = si.name.toStdString();

		for (QScreen *qs : qscreens) {
			if (qs->geometry() == si.geometry) {
				si.screen = qs;
				break;
			}
		}
		if (!si.screen) {
			for (QScreen *qs : qscreens)
				if (qs->name() == si.name) {
					si.screen = qs;
					break;
				}
		}
		if (si.screen && !ns)
			si.dpr = si.screen->devicePixelRatio();
		si.backingW = (int)(si.geometry.width() * si.dpr + 0.5);
		si.backingH = (int)(si.geometry.height() * si.dpr + 0.5);
		if (si.pixelW <= 0) {
			si.pixelW = si.backingW;
			si.pixelH = si.backingH;
		}
		out.push_back(si);
	}
	return out;
}

static NSScreen *nsScreenForQt(QScreen *screen)
{
	if (!screen)
		return nil;
	QRect g = screen->geometry();
	for (NSScreen *s in [NSScreen screens]) {
		NSNumber *n = s.deviceDescription[@"NSScreenNumber"];
		CGRect b = CGDisplayBounds((CGDirectDisplayID)n.unsignedIntValue);
		if ((int)b.origin.x == g.x() && (int)b.origin.y == g.y() && (int)b.size.width == g.width() &&
		    (int)b.size.height == g.height())
			return s;
	}
	return nil;
}

void configureOutputWindow(QWidget *w, QScreen *screen, const OutputWindowOptions &opts)
{
	NSView *view = (__bridge NSView *)reinterpret_cast<void *>(w->winId());
	NSWindow *win = view.window;
	if (!win)
		return;

	/* Above the menu bar and Dock but below screen savers / system alerts. */
	win.level = NSStatusWindowLevel;
	win.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorStationary |
				 NSWindowCollectionBehaviorFullScreenAuxiliary | NSWindowCollectionBehaviorIgnoresCycle;
	win.hasShadow = NO;
	win.opaque = YES;
	win.backgroundColor = NSColor.blackColor;
	win.hidesOnDeactivate = NO;
	win.releasedWhenClosed = NO;
	win.animationBehavior = NSWindowAnimationBehaviorNone;
	if ([win respondsToSelector:@selector(setTabbingMode:)])
		win.tabbingMode = NSWindowTabbingModeDisallowed;

	/* Keep outputs out of screen capture so Program can't contain them. */
	win.sharingType = opts.excludeFromCapture ? NSWindowSharingNone : NSWindowSharingReadOnly;
	(void)opts.allowFocus; /* macOS: activation is controlled by Qt (WA_ShowWithoutActivating) */

	/* Qt constrains frameless windows below the menu bar; set the frame
	 * natively so it covers the full display. */
	if (NSScreen *target = nsScreenForQt(screen))
		[win setFrame:target.frame display:YES];
}

bool outputWindowOnTarget(QWidget *w, QScreen *screen)
{
	if (!screen || !QGuiApplication::screens().contains(screen))
		return false;
	NSView *view = (__bridge NSView *)reinterpret_cast<void *>(w->winId());
	NSWindow *win = view.window;
	NSScreen *target = nsScreenForQt(screen);
	if (!win || !target || win.screen != target)
		return false;
	return NSEqualRects(win.frame, target.frame);
}

QSize nativePixelSize(QWidget *w)
{
	return w->size() * w->devicePixelRatioF();
}

void installDisplayChangeHook(std::function<void()>) {}
void removeDisplayChangeHook() {}

QString modKeyName()
{
	return QStringLiteral("⌘");
}

QString altKeyName()
{
	return QStringLiteral("⌥");
}

static IOPMAssertionID g_assertion = kIOPMNullAssertionID;

void setDisplaySleepInhibited(bool inhibit)
{
	if (inhibit && g_assertion == kIOPMNullAssertionID) {
		IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleDisplaySleep, kIOPMAssertionLevelOn,
					    CFSTR("OBS Output Mapping fullscreen output"), &g_assertion);
	} else if (!inhibit && g_assertion != kIOPMNullAssertionID) {
		IOPMAssertionRelease(g_assertion);
		g_assertion = kIOPMNullAssertionID;
	}
}

} // namespace om
