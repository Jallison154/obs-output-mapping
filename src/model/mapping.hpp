/*
Output Mapping for OBS Studio
Copyright (C) 2026 Okami Designs

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include "core/geometry.hpp"
#include "core/display-match.hpp"

#include <QColor>
#include <QJsonObject>
#include <QString>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace om {

/* A slice grabs `src` (Program canvas pixels) and draws it at `dst`
 * (physical display pixels). */
struct Slice {
	QString id;
	QString name;
	QColor color;
	IRect src;
	IRect dst;
	bool enabled = true;
};

enum class DisplayKind {
	Physical, /* bound to a real monitor/HDMI output by identity      */
	Virtual,  /* no monitor; fixed resolution, windowed output only   */
};

/* One output display and the slices composited onto it. Slices are kept in
 * sidebar order: index 0 is the TOP of the list and is drawn LAST (on top). */
struct DisplayMapping {
	QString id;
	QString name;
	DisplayKind kind = DisplayKind::Physical;
	DisplayIdentity identity;
	int width = 1920; /* physical display pixels (last known for Physical) */
	int height = 1080;
	bool outputWanted = false; /* fullscreen was running: resume when the exact display is present */
	int delayMs = 0;           /* video delay for this output (0 = live), see kMaxDelayMs */
	std::vector<Slice> slices;

	int sliceIndex(const QString &sliceId) const;
};

struct MappingDocument {
	std::vector<DisplayMapping> displays;
	bool restoreOutputs = true;
	bool excludeFromCapture = true; /* hide outputs from screen capture (prevents feedback) */

	int displayIndex(const QString &displayId) const;
	DisplayMapping *display(const QString &displayId);
	const DisplayMapping *display(const QString &displayId) const;
	/* Find the display that owns a slice. */
	DisplayMapping *displayForSlice(const QString &sliceId);
	const DisplayMapping *displayForSlice(const QString &sliceId) const;
	Slice *slice(const QString &sliceId);
	const Slice *slice(const QString &sliceId) const;

	QJsonObject toJson() const;
	static MappingDocument fromJson(const QJsonObject &obj);
};

static constexpr int kMaxDelayMs = 1000;

QColor sliceColorForIndex(int i);
QString newId();

/* ---------------------------------------------------------------------- */
/* Immutable snapshot consumed by the graphics thread.                     */

struct RenderSlice {
	IRect src;
	IRect dst;
};

struct RenderDisplay {
	std::string id;
	int width = 0, height = 0;
	int delayMs = 0;
	std::vector<RenderSlice> slices; /* bottom-most first (draw order) */
};

struct RenderState {
	std::unordered_map<std::string, RenderDisplay> displays;
};

/* Lock-light publication of the latest RenderState for draw callbacks. */
class RenderStore {
public:
	static RenderStore &instance();
	void publish(std::shared_ptr<const RenderState> s);
	std::shared_ptr<const RenderState> get() const;
	bool getDisplay(const std::string &id, RenderDisplay &out) const;

private:
	mutable std::mutex mutex_;
	std::shared_ptr<const RenderState> state_ = std::make_shared<RenderState>();
};

std::shared_ptr<const RenderState> buildRenderState(const MappingDocument &doc);

} // namespace om
