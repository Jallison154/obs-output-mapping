/*
Output Mapping for OBS Studio
Copyright (C) 2026 Okami Designs

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// "Vexel" dock for the main OBS window: a confidence monitor per
// mapped display (live thumbnail of exactly what the display receives,
// including delay and test patterns), status, and start/stop controls.

#pragma once

#include <QImage>
#include <QWidget>

#include <atomic>
#include <functional>
#include <string>

class QCheckBox;
class QVBoxLayout;

namespace om {

/* Thumbnail of one output. A plain widget (no native window, no GPU
 * surface): it paints the latest image rendered by the graphics thread
 * through the same path as the real output (~10 fps). */
class DockPreview : public QWidget {
	Q_OBJECT
public:
	DockPreview(const QString &displayId, int dispW, int dispH, QWidget *parent = nullptr);
	static std::atomic<bool> live; /* "Live previews" switch */

	QString displayId() const { return id_; }
	QSize requestedPixels() const; /* thumbnail size to render */
	void setImage(const QImage &img);

	bool hasHeightForWidth() const override { return true; }
	int heightForWidth(int w) const override;
	QSize sizeHint() const override { return QSize(320, heightForWidth(320)); }

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;

private:
	QString id_;
	double aspect_ = 9.0 / 16.0;
	QImage image_;
};

class OutputDock : public QWidget {
	Q_OBJECT
public:
	explicit OutputDock(std::function<void()> openEditor, QWidget *parent = nullptr);

	/* Screenshot of the dock with live thumbnails composited (automation). */
	QImage captureImage();
	int tileCount() const;
	static std::atomic<int> layoutPasses; /* tile resize events (diagnostics) */

private:
	void rebuild();
	void updateRequests(); /* tell the graphics thread which thumbnails to render */
	void pullImages();
	QWidget *makeTile(const QString &displayId);
	void stopAllConfirmed();

	std::function<void()> openEditor_;
	QVBoxLayout *tiles_ = nullptr;
	QCheckBox *liveCheck_ = nullptr;
};

} // namespace om
