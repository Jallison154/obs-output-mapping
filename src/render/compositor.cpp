/*
Output Mapping for OBS Studio
Copyright (C) 2026 Okami Designs

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "render/compositor.hpp"

#include <obs.h>
#include <graphics/matrix4.h>
#include <graphics/vec4.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <future>

namespace om::gfx {

static gs_samplerstate_t *g_point = nullptr;
static gs_samplerstate_t *g_linear = nullptr;
static gs_vertbuffer_t *g_quad = nullptr;

void init()
{
	if (g_quad)
		return;
	gs_sampler_info si = {};
	si.filter = GS_FILTER_POINT;
	si.address_u = GS_ADDRESS_CLAMP;
	si.address_v = GS_ADDRESS_CLAMP;
	si.address_w = GS_ADDRESS_CLAMP;
	si.max_anisotropy = 1;
	g_point = gs_samplerstate_create(&si);
	si.filter = GS_FILTER_LINEAR;
	g_linear = gs_samplerstate_create(&si);

	gs_render_start(true);
	gs_vertex2f(0.0f, 0.0f);
	gs_vertex2f(1.0f, 0.0f);
	gs_vertex2f(0.0f, 1.0f);
	gs_vertex2f(1.0f, 1.0f);
	g_quad = gs_render_save();
}

void shutdown()
{
	gs_samplerstate_destroy(g_point);
	gs_samplerstate_destroy(g_linear);
	gs_vertexbuffer_destroy(g_quad);
	g_point = g_linear = nullptr;
	g_quad = nullptr;
}

void programSize(uint32_t &w, uint32_t &h)
{
	obs_video_info ovi = {};
	if (obs_get_video_info(&ovi)) {
		w = ovi.base_width;
		h = ovi.base_height;
	} else {
		w = h = 0;
	}
}

bool acquireProgram(ProgramFrame &pf)
{
	pf.tex = obs_get_main_texture();
	if (!pf.tex)
		return false;
	pf.width = gs_texture_get_width(pf.tex);
	pf.height = gs_texture_get_height(pf.tex);

	/* Mirror libobs' obs_render_main_texture() colour handling so mapped
	 * outputs look exactly like OBS projectors. */
	obs_video_info ovi = {};
	obs_get_video_info(&ovi);
	bool hdr = ovi.colorspace == VIDEO_CS_2100_PQ || ovi.colorspace == VIDEO_CS_2100_HLG;
	pf.tech = "Draw";
	pf.multiplier = 1.0f;
	switch (gs_get_color_space()) {
	case GS_CS_SRGB:
	case GS_CS_SRGB_16F:
		if (hdr)
			pf.tech = "DrawTonemap";
		break;
	case GS_CS_709_SCRGB:
		pf.tech = "DrawMultiply";
		pf.multiplier = obs_get_video_sdr_white_level() / 80.0f;
		break;
	case GS_CS_709_EXTENDED:
		break;
	}
	return pf.width > 0 && pf.height > 0;
}

static void drawProgramRegion(const ProgramFrame &pf, const IRect &src, bool pointFilter)
{
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	gs_eparam_t *mult = gs_effect_get_param_by_name(effect, "multiplier");

	gs_effect_set_texture_srgb(image, pf.tex);
	gs_effect_set_float(mult, pf.multiplier);
	gs_effect_set_next_sampler(image, pointFilter ? g_point : g_linear);

	while (gs_effect_loop(effect, pf.tech))
		gs_draw_sprite_subregion(pf.tex, 0, (uint32_t)src.x, (uint32_t)src.y, (uint32_t)src.w, (uint32_t)src.h);
}

void drawProgram(const ProgramFrame &pf, bool pointFilter)
{
	if (!pf.tex)
		return;
	const bool prevSrgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_blend_state_push();
	gs_enable_blending(false);

	drawProgramRegion(pf, {0, 0, (int)pf.width, (int)pf.height}, pointFilter);

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(prevSrgb);
}

void drawSlices(const ProgramFrame &pf, const RenderDisplay &rd)
{
	if (!pf.tex || rd.slices.empty())
		return;
	const bool prevSrgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_blend_state_push();
	gs_enable_blending(false); /* opaque: upper slices replace lower ones */

	for (const RenderSlice &s : rd.slices) {
		ClippedSlice c;
		if (!clipSliceToCanvas(s.src, s.dst, (int)pf.width, (int)pf.height, c))
			continue;
		const double sx = c.dw / c.src.w, sy = c.dh / c.src.h;
		/* Exact 1:1 copies use point sampling so every pixel is copied
		 * verbatim; scaled slices are filtered. */
		const bool oneToOne = std::fabs(sx - 1.0) < 1e-9 && std::fabs(sy - 1.0) < 1e-9 &&
				      std::floor(c.dx) == c.dx && std::floor(c.dy) == c.dy;

		gs_matrix_push();
		gs_matrix_translate3f((float)c.dx, (float)c.dy, 0.0f);
		gs_matrix_scale3f((float)sx, (float)sy, 1.0f);
		drawProgramRegion(pf, c.src, oneToOne);
		gs_matrix_pop();
	}

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(prevSrgb);
}

void renderDisplay(const RenderDisplay &rd, uint32_t cx, uint32_t cy, bool fit)
{
	if (rd.width <= 0 || rd.height <= 0 || cx == 0 || cy == 0)
		return;

	int vx = 0, vy = 0, vw = (int)cx, vh = (int)cy;
	if (fit) {
		double s = std::min((double)cx / rd.width, (double)cy / rd.height);
		vw = std::max(1, (int)std::lround(rd.width * s));
		vh = std::max(1, (int)std::lround(rd.height * s));
		vx = ((int)cx - vw) / 2;
		vy = ((int)cy - vh) / 2;
	}

	gs_viewport_push();
	gs_projection_push();
	gs_matrix_push();
	gs_matrix_identity();

	gs_set_viewport(vx, vy, vw, vh);
	gs_ortho(0.0f, (float)rd.width, 0.0f, (float)rd.height, -100.0f, 100.0f);

	ProgramFrame pf;
	if (acquireProgram(pf))
		drawSlices(pf, rd);

	gs_matrix_pop();
	gs_projection_pop();
	gs_viewport_pop();
}

void setCanvasView(const QRectF &cv, int vpX, int vpY, int vpW, int vpH)
{
	gs_set_viewport(vpX, vpY, vpW, vpH);
	gs_ortho((float)cv.left(), (float)cv.right(), (float)cv.top(), (float)cv.bottom(), -100.0f, 100.0f);
}

void setScreenView(uint32_t cx, uint32_t cy)
{
	gs_set_viewport(0, 0, (int)cx, (int)cy);
	gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
}

static void setColor(gs_effect_t *solid, uint32_t argb)
{
	vec4 c;
	vec4_set(&c, ((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f, (argb & 0xFF) / 255.0f,
		 ((argb >> 24) & 0xFF) / 255.0f);
	gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), &c);
}

void fillRect(float x, float y, float w, float h, uint32_t argb)
{
	if (w <= 0 || h <= 0 || !g_quad)
		return;
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	const bool prevSrgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

	setColor(solid, argb);
	gs_matrix_push();
	gs_matrix_translate3f(x, y, 0.0f);
	gs_matrix_scale3f(w, h, 1.0f);
	gs_load_vertexbuffer(g_quad);
	gs_load_indexbuffer(nullptr);
	while (gs_effect_loop(solid, "Solid"))
		gs_draw(GS_TRISTRIP, 0, 0);
	gs_matrix_pop();

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(prevSrgb);
}

void strokeRect(float x, float y, float w, float h, float t, uint32_t argb)
{
	fillRect(x - t, y - t, w + 2 * t, t, argb); /* top    */
	fillRect(x - t, y + h, w + 2 * t, t, argb); /* bottom */
	fillRect(x - t, y, t, h, argb);             /* left   */
	fillRect(x + w, y, t, h, argb);             /* right  */
}

void drawLine(float x0, float y0, float x1, float y1, float t, uint32_t argb)
{
	/* Only axis-aligned guides are needed. */
	if (std::fabs(x1 - x0) >= std::fabs(y1 - y0))
		fillRect(std::min(x0, x1), y0 - t / 2, std::fabs(x1 - x0), t, argb);
	else
		fillRect(x0 - t / 2, std::min(y0, y1), t, std::fabs(y1 - y0), argb);
}

void drawTexture(gs_texture_t *tex, float x, float y, float w, float h)
{
	if (!tex)
		return;
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	const bool prevSrgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), tex);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "multiplier"), 1.0f);
	gs_matrix_push();
	gs_matrix_translate3f(x, y, 0.0f);
	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(tex, 0, (uint32_t)w, (uint32_t)h);
	gs_matrix_pop();

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(prevSrgb);
}

gs_texture_t *createTextureFromImage(const QImage &img)
{
	if (img.isNull())
		return nullptr;
	QImage rgba = img.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
	const uint8_t *data = rgba.constBits();
	return gs_texture_create((uint32_t)rgba.width(), (uint32_t)rgba.height(), GS_RGBA, 1, &data, 0);
}

static QImage renderToImage(uint32_t w, uint32_t h, const std::function<void(uint32_t, uint32_t)> &draw)
{
	QImage out;
	gs_texrender_t *tr = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	gs_stagesurf_t *stage = gs_stagesurface_create(w, h, GS_BGRA);

	if (gs_texrender_begin_with_color_space(tr, w, h, GS_CS_SRGB)) {
		vec4 black;
		vec4_set(&black, 0.0f, 0.0f, 0.0f, 1.0f);
		gs_clear(GS_CLEAR_COLOR, &black, 0.0f, 0);
		draw(w, h);
		gs_texrender_end(tr);

		gs_stage_texture(stage, gs_texrender_get_texture(tr));
		uint8_t *data = nullptr;
		uint32_t linesize = 0;
		if (gs_stagesurface_map(stage, &data, &linesize)) {
			out = QImage((int)w, (int)h, QImage::Format_RGB32);
			for (uint32_t y = 0; y < h; y++)
				memcpy(out.scanLine((int)y), data + (size_t)y * linesize, (size_t)w * 4);
			gs_stagesurface_unmap(stage);
		}
	}

	gs_stagesurface_destroy(stage);
	gs_texrender_destroy(tr);
	return out;
}

QImage readbackDisplay(const RenderDisplay &rd)
{
	if (rd.width <= 0 || rd.height <= 0)
		return QImage();
	return renderToImage((uint32_t)rd.width, (uint32_t)rd.height,
			     [&](uint32_t cx, uint32_t cy) { renderDisplay(rd, cx, cy, false); });
}

struct CaptureJob {
	uint32_t w = 0, h = 0;
	std::function<void(uint32_t, uint32_t)> draw;
	std::promise<QImage> promise;
	std::atomic<bool> done{false};
};

static void captureRendered(void *param)
{
	auto *job = static_cast<CaptureJob *>(param);
	if (job->done.exchange(true))
		return;
	job->promise.set_value(renderToImage(job->w, job->h, job->draw));
}

QImage captureOffscreen(uint32_t w, uint32_t h, std::function<void(uint32_t, uint32_t)> draw, QString *error)
{
	if (w == 0 || h == 0) {
		if (error)
			*error = QStringLiteral("Empty target.");
		return QImage();
	}
	CaptureJob job;
	job.w = w;
	job.h = h;
	job.draw = std::move(draw);
	std::future<QImage> f = job.promise.get_future();
	obs_add_main_rendered_callback(captureRendered, &job);
	bool ok = f.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
	obs_remove_main_rendered_callback(captureRendered, &job);
	if (!ok) {
		job.done = true; /* a late callback must not touch the stack job */
		if (error)
			*error = QStringLiteral("Timed out waiting for a rendered frame.");
		return QImage();
	}
	return f.get();
}

} // namespace om::gfx
