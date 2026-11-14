/*
Output Mapping for OBS Studio
Copyright (C) 2026 Okami Designs

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// The Vexel mark: a "V" built from eight display tiles, cyan -> blue -> purple
// (Okami Designs brand guide). Drawn as vectors so it is sharp at any size.

#pragma once

#include <QIcon>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPolygonF>

#include <algorithm>
#include <initializer_list>

namespace om::logo {

inline const char *kCyan = "#19D3F5";
inline const char *kBlue = "#2F6BFF";
inline const char *kPurple = "#8A5CFF";

/* Draws the mark into `r` (square-ish), optionally on the dark app-icon tile. */
inline void paintMark(QPainter &p, const QRectF &r, bool tile = false)
{
	p.save();
	p.setRenderHint(QPainter::Antialiasing, true);
	if (tile) {
		const qreal rad = r.width() * 0.22;
		QLinearGradient bg(r.topLeft(), r.bottomRight());
		bg.setColorAt(0.0, QColor("#0E1A33"));
		bg.setColorAt(1.0, QColor("#050914"));
		p.setPen(QPen(QColor(kCyan), std::max<qreal>(1.0, r.width() * 0.03)));
		p.setBrush(bg);
		const qreal in = r.width() * 0.02;
		p.drawRoundedRect(r.adjusted(in, in, -in, -in), rad, rad);
	}
	/* Mark geometry on a 100x100 grid; the right wing mirrors the left. */
	const qreal pad = tile ? 0.16 : 0.0;
	const QRectF m(r.x() + r.width() * pad, r.y() + r.height() * pad, r.width() * (1 - 2 * pad),
		       r.height() * (1 - 2 * pad));
	auto pt = [&](qreal x, qreal y) {
		return QPointF(m.x() + m.width() * x / 100.0, m.y() + m.height() * y / 100.0);
	};
	/* Traced from the brand artwork: four tiles per wing (outer and inner,
	 * top and bottom); the inner tiles slant in to the V's point. */
	const std::initializer_list<std::initializer_list<QPointF>> left = {
		{{0.4, 14.9}, {23.4, 27.3}, {23.4, 43.2}, {0.4, 37.5}},   /* outer top */
		{{25.1, 27.7}, {40.0, 35.4}, {48.5, 62.4}, {25.1, 50.7}}, /* inner top */
		{{6.4, 44.3}, {23.4, 51.7}, {23.4, 71.9}, {6.4, 64.5}},   /* outer bottom */
		{{25.1, 52.4}, {48.9, 63.9}, {48.9, 85.8}, {25.1, 74.1}}, /* inner bottom */
	};
	/* Cyan (left) -> blue -> purple (right), deepening to blue lower down. */
	QLinearGradient g(pt(0, 0), pt(100, 0));
	g.setColorAt(0.0, QColor("#22E6FF"));
	g.setColorAt(0.5, QColor(kBlue));
	g.setColorAt(1.0, QColor(kPurple));
	QLinearGradient shade(pt(0, 15), pt(0, 86));
	shade.setColorAt(0.0, QColor(47, 107, 255, 0));
	shade.setColorAt(1.0, QColor(40, 80, 255, 110));
	p.setPen(Qt::NoPen);
	for (const QBrush &brush : {QBrush(g), QBrush(shade)}) {
		p.setBrush(brush);
		for (bool mirror : {false, true}) {
			for (const auto &poly : left) {
				QPolygonF q;
				for (const QPointF &v : poly)
					q << pt(mirror ? 100.0 - v.x() : v.x(), v.y());
				p.drawPolygon(q);
			}
		}
	}
	p.restore();
}

inline QPixmap markPixmap(int size, qreal dpr = 1.0, bool tile = false)
{
	QPixmap pm(QSize(size, size) * dpr);
	pm.setDevicePixelRatio(dpr);
	pm.fill(Qt::transparent);
	QPainter p(&pm);
	paintMark(p, QRectF(0, 0, size, size), tile);
	return pm;
}

inline QIcon appIcon()
{
	QIcon icon;
	for (int s : {16, 24, 32, 48, 64, 128, 256})
		icon.addPixmap(markPixmap(s, 1.0, true));
	return icon;
}

} // namespace om::logo
