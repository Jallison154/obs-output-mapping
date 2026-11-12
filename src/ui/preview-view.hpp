/*
Output Mapping for OBS Studio
Copyright (C) 2026 Okami Designs

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Live, interactive preview used for both editor panes:
//  - Input:  the OBS Program canvas; draw/move/resize slice SOURCE crops.
//  - Output: one display at its real aspect, showing the composed result;
//            move/resize slice DESTINATION rectangles.
// Video and overlays are drawn on the GPU by OBS' graphics thread. The
// overlays (outlines, labels, handles, guides) exist only here – never in
// the fullscreen outputs.

#pragma once

#include "core/geometry.hpp"
#include "ui/obs-display-widget.hpp"

#include <QImage>
#include <QPointF>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace om {

class PreviewView : public ObsDisplayWidget {
	Q_OBJECT

public:
	enum class Kind { Input, Output };

	PreviewView(Kind kind, QWidget *parent = nullptr);
	~PreviewView() override;

	Kind kind() const { return kind_; }

	void fitToView();
	void zoomActual();
	void zoomBy(double factor);
	double zoom() const { return zoom_; }

	void setSnapping(bool on) { snapping_ = on; }
	void setAspectLock(bool on) { aspectLock_ = on; }
	bool snapping() const { return snapping_; }
	bool aspectLock() const { return aspectLock_; }

	/* Canvas (Program px / display px) <-> widget logical coordinates. */
	QPointF canvasToWidget(const QPointF &c) const;
	QPointF widgetToCanvas(const QPointF &w) const;

	/* Drive the real mouse handlers with synthetic events (automation). */
	void simulateDrag(const QPointF &fromCanvas, const QPointF &toCanvas, Qt::KeyboardModifiers mods,
			  int steps = 12);

	/* Render this preview (video + editor overlays) offscreen at its
	 * current pixel size, exactly as drawn on screen. */
	QImage capture();

	/* Re-snapshot overlay state for the render thread. */
	void refresh();

signals:
	void zoomChanged(double zoom);
	void hoverChanged(QPointF canvasPos, bool inside);
	void needDisplay(); /* user tried to draw with no display */

protected:
	void renderFrame(uint32_t cx, uint32_t cy) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void mouseDoubleClickEvent(QMouseEvent *e) override;
	void wheelEvent(QWheelEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	void keyReleaseEvent(QKeyEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void leaveEvent(QEvent *e) override;
	bool event(QEvent *e) override;

private:
	struct Item {
		QString sliceId;
		QString displayId;
		IRect rect;
		uint32_t color = 0;
		bool selected = false;
		bool dim = false;
	};

	struct OverlayItem {
		IRect rect;
		uint32_t color = 0;
		bool selected = false, hovered = false, dim = false;
		std::string labelKey;
		QImage label;
	};

	struct Overlay {
		int canvasW = 0, canvasH = 0;
		double zoom = 1, panX = 0, panY = 0; /* device pixels */
		double dpr = 1;
		std::string displayId;
		std::vector<OverlayItem> items;
		bool guideX = false, guideY = false;
		double guideXPos = 0, guideYPos = 0;
		bool rubber = false;
		IRect rubberRect;
		std::string centerKey;
		QImage centerLabel;
	};

	enum class Drag { None, Pan, Create, Move, Resize };

	void canvasSize(int &w, int &h) const;
	std::vector<Item> items() const;
	IRect sliceRect(const QString &sliceId) const;
	void applyRect(const QString &sliceId, const IRect &r);
	SnapTargets snapTargets(const QString &exclude) const;
	QPointF eventCanvasPos(const QPointF &widgetPos) const;
	void updateHover(const QPointF &canvasPos);
	void setZoomAt(double z, const QPointF &anchorDevice);
	void clampPan();
	void nudge(int dx, int dy);
	QImage makeLabel(const QString &text, const QColor &color, bool selected) const;
	void drawOverlay(const Overlay &ov, uint32_t cx, uint32_t cy);

	Kind kind_;
	double zoom_ = 1.0, panX_ = 0, panY_ = 0; /* device px */
	bool autoFit_ = true;
	bool snapping_ = true;
	bool aspectLock_ = false;
	bool spaceDown_ = false;

	Drag drag_ = Drag::None;
	QPointF dragStartCanvas_, dragStartDevice_;
	double dragStartPanX_ = 0, dragStartPanY_ = 0;
	IRect dragStartRect_;
	Handle dragHandle_ = Handle::None;
	QString dragSlice_;
	IRect rubber_;
	bool guideX_ = false, guideY_ = false;
	double guideXPos_ = 0, guideYPos_ = 0;
	QString hoverSlice_;

	std::mutex overlayMutex_;
	Overlay overlay_;

	/* graphics-thread owned label textures */
	std::unordered_map<std::string, struct gs_texture *> labelCache_;
};

} // namespace om
