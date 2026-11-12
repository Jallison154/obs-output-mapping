/*
Output Mapping for OBS Studio
Copyright (C) 2026 Okami Designs

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include "platform/platform.hpp"
#include "ui/obs-display-widget.hpp"

#include <QPointer>
#include <QScreen>

#include <string>

namespace om {

/* A mapped output: borderless fullscreen on one physical display, or a
 * normal resizable window (testing / virtual displays). Renders only the
 * composed video – no editor overlays. */
class OutputWindow : public ObsDisplayWidget {
	Q_OBJECT

public:
	enum class Mode { Fullscreen, Window };

	OutputWindow(const QString &displayId, Mode mode, QScreen *screen, const QString &title, int dispW, int dispH);
	~OutputWindow() override;

	Mode mode() const { return mode_; }
	QScreen *targetScreen() const { return screen_; }
	QString displayId() const { return QString::fromStdString(displayId_); }

	void showOutput(const OutputWindowOptions &opts);
	void reapplyPlacement();
	int misplacedChecks = 0; /* consecutive watchdog failures */

signals:
	void closeRequested(const QString &displayId);

protected:
	void renderFrame(uint32_t cx, uint32_t cy) override;
	void keyPressEvent(QKeyEvent *e) override;
	void closeEvent(QCloseEvent *e) override;

private:
	std::string displayId_;
	Mode mode_;
	QPointer<QScreen> screen_;
	int dispW_, dispH_;
	bool closingFromCode_ = false;
	bool allowFocus_ = false;
	bool excludeFromCapture_ = true;

	friend class Controller;
};

} // namespace om
