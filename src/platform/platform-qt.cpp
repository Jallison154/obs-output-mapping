/*
Output Mapping for OBS Studio
Copyright (C) 2026 Okami Designs

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Generic QScreen-based fallback for Windows / Linux. Not verified on
// those platforms yet (see README "Platform status").

#include "platform/platform.hpp"

#include <QCryptographicHash>
#include <QGuiApplication>
#include <QScreen>
#include <QWidget>

namespace om {

std::vector<ScreenInfo> enumerateScreens()
{
	std::vector<ScreenInfo> out;
	QScreen *primary = QGuiApplication::primaryScreen();
	for (QScreen *qs : QGuiApplication::screens()) {
		ScreenInfo si;
		si.screen = qs;
		si.geometry = qs->geometry();
		si.dpr = qs->devicePixelRatio();
		si.backingW = (int)(si.geometry.width() * si.dpr + 0.5);
		si.backingH = (int)(si.geometry.height() * si.dpr + 0.5);
		si.pixelW = si.backingW;
		si.pixelH = si.backingH;
		si.nativeId = qs->name();
		si.primary = qs == primary;
		QString label = qs->manufacturer() + " " + qs->model();
		si.name = label.trimmed().isEmpty() ? qs->name() : label.trimmed();
		/* Qt has no stable OS id; derive one from connector name + EDID text. */
		QByteArray key =
			(qs->name() + "|" + qs->manufacturer() + "|" + qs->model() + "|" + qs->serialNumber()).toUtf8();
		si.identity.uuid =
			"qt:" + QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex().toStdString();
		si.identity.vendor = (uint32_t)qHash(qs->manufacturer());
		si.identity.model = (uint32_t)qHash(qs->model());
		si.identity.serial = qs->serialNumber().isEmpty() ? 0u : (uint32_t)qHash(qs->serialNumber());
		si.identity.name = si.name.toStdString();
		out.push_back(si);
	}
	return out;
}

void configureOutputWindow(QWidget *w, QScreen *screen, const OutputWindowOptions &)
{
	if (screen)
		w->setGeometry(screen->geometry());
}

bool outputWindowOnTarget(QWidget *w, QScreen *screen)
{
	return screen && QGuiApplication::screens().contains(screen) && w->screen() == screen;
}

QSize nativePixelSize(QWidget *w)
{
	return w->size() * w->devicePixelRatioF();
}

void installDisplayChangeHook(std::function<void()>) {}
void removeDisplayChangeHook() {}
void setDisplaySleepInhibited(bool) {}

QString modKeyName()
{
	return QStringLiteral("Ctrl");
}

QString altKeyName()
{
	return QStringLiteral("Alt");
}

} // namespace om
