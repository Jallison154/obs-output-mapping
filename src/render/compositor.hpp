/*
Output Mapping for OBS Studio
Copyright (C) 2026 Okami Designs

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// GPU compositing of Program slices. Everything here must be called with
// the OBS graphics context entered (draw callbacks already are).

#pragma once

#include "model/mapping.hpp"

#include <graphics/graphics.h>

#include <QImage>
#include <QRectF>

#include <functional>
#include <vector>
#include <string>

namespace om::gfx {

/* Create/destroy shared GPU objects (call with graphics entered). */
void init();
void shutdown();

/* The live Program frame (OBS main canvas render texture, post-transition). */
struct ProgramFrame {
	gs_texture_t *tex = nullptr;
	uint32_t width = 0, height = 0;
	const char *tech = "Draw";
	float multiplier = 1.0f;
};
bool acquireProgram(ProgramFrame &pf);
/* Base canvas size even when no frame is available yet. */
void programSize(uint32_t &w, uint32_t &h);

/* Draw the whole Program texture at (0,0)-(w,h) in current coordinates. */
void drawProgram(const ProgramFrame &pf, bool pointFilter);

/* Composite a display's slices in display-pixel coordinates (current
 * projection must map display pixels). Bottom-most slice first. */
void drawSlices(const ProgramFrame &pf, const RenderDisplay &rd);

/* What a display shows right now, without delay: live slices, or the
 * identify overlay (test pattern / slice outlines) when it is on. Used by
 * the editor's Output Placement preview. Display-pixel coordinates. */
void drawDisplayLive(const ProgramFrame &pf, const RenderDisplay &rd);

/* Render a display into the current target sized cx x cy.
 * fit=false: stretch display pixels to the target (fullscreen: 1:1 when sizes match).
 * fit=true : letterbox, preserving aspect (windowed outputs). */
void renderDisplay(const RenderDisplay &rd, uint32_t cx, uint32_t cy, bool fit);

/* Set viewport+projection so that canvas rect `canvasVisible` (canvas
 * units) lands on integer target rect `vp` (target pixels). Used by the
 * editor to clip drawing to the canvas without scissor rects. */
void setCanvasView(const QRectF &canvasVisible, int vpX, int vpY, int vpW, int vpH);
void setScreenView(uint32_t cx, uint32_t cy);

/* Overlay primitives (screen space, straight alpha colour 0xAARRGGBB). */
void fillRect(float x, float y, float w, float h, uint32_t argb);
void strokeRect(float x, float y, float w, float h, float thickness, uint32_t argb);
void drawLine(float x0, float y0, float x1, float y1, float thickness, uint32_t argb);
/* Premultiplied RGBA texture (e.g. uploaded label). */
void drawTexture(gs_texture_t *tex, float x, float y, float w, float h);
gs_texture_t *createTextureFromImage(const QImage &img);

/* Offscreen render of a display mapping, exactly like the fullscreen
 * output, read back to CPU. Must be called from the graphics thread
 * inside a frame (e.g. a main-rendered callback). */
QImage readbackDisplay(const RenderDisplay &rd);

/* Output video delay: frames used for a delay at the current OBS frame
 * rate (0 = live), and that frame rate. */
int delayFrames(int delayMs);
double videoFps();

/* Render `draw(cx, cy)` into an offscreen w x h target on the graphics
 * thread during the next frame and return it. Call from any non-graphics
 * thread (typically the UI thread); blocks up to ~3 s. */
QImage captureOffscreen(uint32_t w, uint32_t h, std::function<void(uint32_t, uint32_t)> draw, QString *error = nullptr);

/* ---- Thumbnails for the OBS dock (no native windows) ----
 * The UI declares which displays it wants as thumbnails and at what size;
 * the graphics thread renders them through renderDisplay() (same as the
 * outputs) at ~10 fps with double-buffered, non-blocking readback. */
struct ThumbnailRequest {
	std::string displayId;
	uint32_t width = 0, height = 0;
};
void setThumbnailRequests(std::vector<ThumbnailRequest> requests);
QImage latestThumbnail(const std::string &displayId);

} // namespace om::gfx
