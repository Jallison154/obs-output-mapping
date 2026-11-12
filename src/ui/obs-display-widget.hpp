/*
Output Mapping for OBS Studio
Copyright (C) 2026 Okami Designs

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// A native QWidget hosting an obs_display (separate swapchain rendered by
// OBS' graphics thread every frame). Same approach as OBS' own
// OBSQTDisplay, which is not exported to plugins.

#pragma once

#include <obs.h>

#include <QWidget>

#include <atomic>

namespace om {

class ObsDisplayWidget : public QWidget {
	Q_OBJECT

public:
	explicit ObsDisplayWidget(QWidget *parent = nullptr, Qt::WindowFlags flags = Qt::WindowFlags());
	~ObsDisplayWidget() override;

	void destroyDisplay();
	obs_display_t *display() const { return display_; }
	QSize pixelSize() const;
	void setBackground(uint32_t rgb) { background_ = rgb; }

protected:
	/* Called on the OBS graphics thread with a cx x cy swapchain bound. */
	virtual void renderFrame(uint32_t cx, uint32_t cy) = 0;

	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void showEvent(QShowEvent *e) override;
	QPaintEngine *paintEngine() const override;
	bool eventFilter(QObject *obj, QEvent *e) override;

private:
	void createDisplay();
	static void drawCallback(void *param, uint32_t cx, uint32_t cy);

	obs_display_t *display_ = nullptr;
	uint32_t background_ = 0x000000;
	bool destroying_ = false;
};

} // namespace om
