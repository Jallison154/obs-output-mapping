/*
Output Mapping for OBS Studio
Copyright (C) 2026 Okami Designs

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Pure geometry used by the editor and the compositor. No OBS / Qt
// dependencies so it can be unit tested standalone (see tests/).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace om {

struct IRect {
	int x = 0, y = 0, w = 0, h = 0;

	int left() const { return x; }
	int top() const { return y; }
	int right() const { return x + w; }
	int bottom() const { return y + h; }
	bool empty() const { return w <= 0 || h <= 0; }
	bool contains(double px, double py) const { return px >= x && py >= y && px < x + w && py < y + h; }
	bool operator==(const IRect &o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
	bool operator!=(const IRect &o) const { return !(*this == o); }
};

inline IRect intersect(const IRect &a, const IRect &b)
{
	int l = std::max(a.x, b.x), t = std::max(a.y, b.y);
	int r = std::min(a.right(), b.right()), btm = std::min(a.bottom(), b.bottom());
	if (r <= l || btm <= t)
		return {};
	return {l, t, r - l, btm - t};
}

/* Normalised rectangle from two arbitrary corner points (used when drawing). */
inline IRect rectFromPoints(double x0, double y0, double x1, double y1)
{
	int l = (int)std::lround(std::min(x0, x1)), r = (int)std::lround(std::max(x0, x1));
	int t = (int)std::lround(std::min(y0, y1)), b = (int)std::lround(std::max(y0, y1));
	return {l, t, r - l, b - t};
}

/* Scale `r` down (never up) to fit inside a w x h box, keeping aspect, anchored at 0,0. */
inline IRect fitInside(int srcW, int srcH, int boxW, int boxH)
{
	if (srcW <= 0 || srcH <= 0 || boxW <= 0 || boxH <= 0)
		return {0, 0, std::max(boxW, 1), std::max(boxH, 1)};
	if (srcW <= boxW && srcH <= boxH)
		return {0, 0, srcW, srcH};
	double s = std::min((double)boxW / srcW, (double)boxH / srcH);
	return {0, 0, std::max(1, (int)std::lround(srcW * s)), std::max(1, (int)std::lround(srcH * s))};
}

/* Default destination for a freshly drawn slice: 1:1 at the top-left, scaled
 * down to fit if larger than the display. */
inline IRect defaultDestination(const IRect &src, int dispW, int dispH)
{
	return fitInside(src.w, src.h, dispW, dispH);
}

/* "Fit to Display": largest rect with the source aspect, centred on the display. */
inline IRect fitToDisplay(const IRect &src, int dispW, int dispH)
{
	if (src.empty() || dispW <= 0 || dispH <= 0)
		return {0, 0, dispW, dispH};
	double s = std::min((double)dispW / src.w, (double)dispH / src.h);
	int w = std::max(1, (int)std::lround(src.w * s));
	int h = std::max(1, (int)std::lround(src.h * s));
	return {(dispW - w) / 2, (dispH - h) / 2, w, h};
}

/* ---------------------------------------------------------------------- */
/* Handles                                                                */

enum class Handle : int {
	None = 0,
	Move,
	TopLeft,
	Top,
	TopRight,
	Right,
	BottomRight,
	Bottom,
	BottomLeft,
	Left,
};

inline bool handleMovesLeft(Handle h)
{
	return h == Handle::TopLeft || h == Handle::Left || h == Handle::BottomLeft;
}
inline bool handleMovesRight(Handle h)
{
	return h == Handle::TopRight || h == Handle::Right || h == Handle::BottomRight;
}
inline bool handleMovesTop(Handle h)
{
	return h == Handle::TopLeft || h == Handle::Top || h == Handle::TopRight;
}
inline bool handleMovesBottom(Handle h)
{
	return h == Handle::BottomLeft || h == Handle::Bottom || h == Handle::BottomRight;
}

struct PointD {
	double x = 0, y = 0;
};

/* Handle anchor positions in canvas units (8 resize handles). */
inline PointD handlePos(const IRect &r, Handle h)
{
	double cx = r.x + r.w / 2.0, cy = r.y + r.h / 2.0;
	switch (h) {
	case Handle::TopLeft:
		return {(double)r.x, (double)r.y};
	case Handle::Top:
		return {cx, (double)r.y};
	case Handle::TopRight:
		return {(double)r.right(), (double)r.y};
	case Handle::Right:
		return {(double)r.right(), cy};
	case Handle::BottomRight:
		return {(double)r.right(), (double)r.bottom()};
	case Handle::Bottom:
		return {cx, (double)r.bottom()};
	case Handle::BottomLeft:
		return {(double)r.x, (double)r.bottom()};
	case Handle::Left:
		return {(double)r.x, cy};
	default:
		return {cx, cy};
	}
}

static constexpr Handle kResizeHandles[] = {Handle::TopLeft,     Handle::Top,    Handle::TopRight,   Handle::Right,
					    Handle::BottomRight, Handle::Bottom, Handle::BottomLeft, Handle::Left};

/* Hit test in canvas units. `tol` is the grab radius in canvas units
 * (screen pixels / zoom). Resize handles win over the body. */
inline Handle hitTest(const IRect &r, double px, double py, double tol)
{
	for (Handle h : kResizeHandles) {
		PointD p = handlePos(r, h);
		if (std::fabs(px - p.x) <= tol && std::fabs(py - p.y) <= tol)
			return h;
	}
	/* Edges are grabbable along their whole length too. */
	bool inY = py >= r.y - tol && py <= r.bottom() + tol;
	bool inX = px >= r.x - tol && px <= r.right() + tol;
	if (inY && std::fabs(px - r.x) <= tol * 0.6)
		return Handle::Left;
	if (inY && std::fabs(px - r.right()) <= tol * 0.6)
		return Handle::Right;
	if (inX && std::fabs(py - r.y) <= tol * 0.6)
		return Handle::Top;
	if (inX && std::fabs(py - r.bottom()) <= tol * 0.6)
		return Handle::Bottom;
	if (r.contains(px, py))
		return Handle::Move;
	return Handle::None;
}

/* ---------------------------------------------------------------------- */
/* Snapping                                                               */

struct SnapTargets {
	std::vector<double> xs; /* candidate vertical lines (canvas units)   */
	std::vector<double> ys; /* candidate horizontal lines (canvas units) */
};

struct SnapResult {
	double delta = 0; /* correction to apply                   */
	double line = 0;  /* the line snapped to (for guides)      */
	bool snapped = false;
};

/* Find the closest candidate to any of `values` within `threshold`. */
inline SnapResult snapAxis(const std::vector<double> &values, const std::vector<double> &targets, double threshold)
{
	SnapResult best;
	double bestDist = threshold;
	for (double v : values) {
		for (double t : targets) {
			double d = std::fabs(t - v);
			if (d <= bestDist) {
				bestDist = d;
				best.delta = t - v;
				best.line = t;
				best.snapped = true;
			}
		}
	}
	return best;
}

struct DragResult {
	IRect rect;
	bool snappedX = false, snappedY = false;
	double guideX = 0, guideY = 0;
};

/* Move `start` by (dx,dy) canvas units, snapping its left/centre/right and
 * top/centre/bottom edges to targets. */
inline DragResult applyMove(const IRect &start, double dx, double dy, const SnapTargets *snap, double threshold)
{
	double nx = start.x + dx, ny = start.y + dy;
	DragResult res;
	if (snap) {
		SnapResult sx = snapAxis({nx, nx + start.w / 2.0, nx + start.w}, snap->xs, threshold);
		SnapResult sy = snapAxis({ny, ny + start.h / 2.0, ny + start.h}, snap->ys, threshold);
		if (sx.snapped) {
			nx += sx.delta;
			res.snappedX = true;
			res.guideX = sx.line;
		}
		if (sy.snapped) {
			ny += sy.delta;
			res.snappedY = true;
			res.guideY = sy.line;
		}
	}
	res.rect = {(int)std::lround(nx), (int)std::lround(ny), start.w, start.h};
	return res;
}

/* Resize `start` by dragging handle `h` by (dx,dy). Moving edges snap to
 * targets. With `lockAspect`, the aspect ratio of `start` is preserved
 * (anchored at the opposite corner / edge centre). */
inline DragResult applyResize(const IRect &start, Handle h, double dx, double dy, bool lockAspect,
			      const SnapTargets *snap, double threshold, int minSize = 1)
{
	double l = start.x, t = start.y, r = start.right(), b = start.bottom();
	DragResult res;

	if (handleMovesLeft(h))
		l += dx;
	if (handleMovesRight(h))
		r += dx;
	if (handleMovesTop(h))
		t += dy;
	if (handleMovesBottom(h))
		b += dy;

	if (snap && !lockAspect) {
		std::vector<double> vx, vy;
		if (handleMovesLeft(h))
			vx.push_back(l);
		if (handleMovesRight(h))
			vx.push_back(r);
		if (handleMovesTop(h))
			vy.push_back(t);
		if (handleMovesBottom(h))
			vy.push_back(b);
		SnapResult sx = snapAxis(vx, snap->xs, threshold);
		SnapResult sy = snapAxis(vy, snap->ys, threshold);
		if (sx.snapped) {
			if (handleMovesLeft(h))
				l += sx.delta;
			else
				r += sx.delta;
			res.snappedX = true;
			res.guideX = sx.line;
		}
		if (sy.snapped) {
			if (handleMovesTop(h))
				t += sy.delta;
			else
				b += sy.delta;
			res.snappedY = true;
			res.guideY = sy.line;
		}
	}

	/* Enforce minimum size without flipping. */
	if (r - l < minSize) {
		if (handleMovesLeft(h))
			l = r - minSize;
		else
			r = l + minSize;
	}
	if (b - t < minSize) {
		if (handleMovesTop(h))
			t = b - minSize;
		else
			b = t + minSize;
	}

	if (lockAspect && start.w > 0 && start.h > 0) {
		double aspect = (double)start.w / start.h;
		double w = r - l, hgt = b - t;
		bool horiz = handleMovesLeft(h) || handleMovesRight(h);
		bool vert = handleMovesTop(h) || handleMovesBottom(h);
		if (horiz && vert) {
			/* Corner: follow whichever axis moved more, relative to size. */
			if (w / start.w >= hgt / start.h)
				hgt = w / aspect;
			else
				w = hgt * aspect;
		} else if (horiz) {
			hgt = w / aspect;
		} else {
			w = hgt * aspect;
		}
		w = std::max(w, (double)minSize);
		hgt = std::max(hgt, (double)minSize);
		/* Anchor at the opposite side; centre on untouched axis. */
		if (handleMovesLeft(h))
			l = r - w;
		else if (handleMovesRight(h))
			r = l + w;
		else {
			double cx = (start.x + start.right()) / 2.0;
			l = cx - w / 2;
			r = cx + w / 2;
		}
		if (handleMovesTop(h))
			t = b - hgt;
		else if (handleMovesBottom(h))
			b = t + hgt;
		else {
			double cy = (start.y + start.bottom()) / 2.0;
			t = cy - hgt / 2;
			b = cy + hgt / 2;
		}
	}

	int il = (int)std::lround(l), it = (int)std::lround(t);
	int ir = (int)std::lround(r), ib = (int)std::lround(b);
	res.rect = {il, it, std::max(minSize, ir - il), std::max(minSize, ib - it)};
	return res;
}

/* Apply a W or H edit from the numeric fields, keeping aspect if locked. */
inline IRect applySizeEdit(const IRect &r, int newW, int newH, bool widthChanged, bool lockAspect)
{
	IRect out = r;
	out.w = std::max(1, newW);
	out.h = std::max(1, newH);
	if (lockAspect && r.w > 0 && r.h > 0) {
		double aspect = (double)r.w / r.h;
		if (widthChanged)
			out.h = std::max(1, (int)std::lround(out.w / aspect));
		else
			out.w = std::max(1, (int)std::lround(out.h * aspect));
	}
	return out;
}

/* ---------------------------------------------------------------------- */
/* Compositing                                                            */

/* Clip a slice's source crop to the program canvas and shrink its
 * destination by the same proportion, so pixels outside Program are simply
 * not drawn (black) instead of being stretched/clamped. Returns false if
 * nothing remains. */
struct ClippedSlice {
	IRect src;
	double dx = 0, dy = 0, dw = 0, dh = 0;
};

inline bool clipSliceToCanvas(const IRect &src, const IRect &dst, int canvasW, int canvasH, ClippedSlice &out)
{
	if (src.empty() || dst.empty())
		return false;
	IRect c = intersect(src, {0, 0, canvasW, canvasH});
	if (c.empty())
		return false;
	double sx = (double)dst.w / src.w, sy = (double)dst.h / src.h;
	out.src = c;
	out.dx = dst.x + (c.x - src.x) * sx;
	out.dy = dst.y + (c.y - src.y) * sy;
	out.dw = c.w * sx;
	out.dh = c.h * sy;
	return true;
}

} // namespace om
