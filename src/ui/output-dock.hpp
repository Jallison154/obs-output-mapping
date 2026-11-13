/*
Output Mapping for OBS Studio
Copyright (C) 2026 Okami Designs

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// "Output Mapping" dock for the main OBS window: a confidence monitor per
// mapped display (live thumbnail of exactly what the display receives,
// including delay and test patterns), status, and start/stop controls.

#pragma once

#include "ui/obs-display-widget.hpp"

#include <QImage>
#include <QWidget>

#include <atomic>
#include <functional>
#include <string>

class QCheckBox;
class QVBoxLayout;

namespace om {

/* Thumbnail of one output, rendered like the real output. */
class DockPreview : public ObsDisplayWidget {
	Q_OBJECT
public:
	DockPreview(const QString &displayId, QWidget *parent = nullptr);
	~DockPreview() override;
	static std::atomic<bool> live; /* "Live previews" switch */
	QImage capture();              /* offscreen render of the thumbnail (automation) */

protected:
	void renderFrame(uint32_t cx, uint32_t cy) override;

private:
	std::string id_;
};

class OutputDock : public QWidget {
	Q_OBJECT
public:
	explicit OutputDock(std::function<void()> openEditor, QWidget *parent = nullptr);

	/* Screenshot of the dock with live thumbnails composited (automation). */
	QImage captureImage();
	int tileCount() const;

private:
	void rebuild();
	QWidget *makeTile(const QString &displayId);
	void stopAllConfirmed();

	std::function<void()> openEditor_;
	QVBoxLayout *tiles_ = nullptr;
	QCheckBox *liveCheck_ = nullptr;
};

} // namespace om
