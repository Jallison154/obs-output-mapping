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
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <future>

namespace om::gfx {

static gs_samplerstate_t *g_point = nullptr;
static gs_samplerstate_t *g_linear = nullptr;
static gs_vertbuffer_t *g_quad = nullptr;

/* ---------------------------------------------------------------------- */
/* Per-display video delay (graphics thread only)                          */
/*                                                                        */
/* Each delayed display owns a ring of N+1 render targets. Once per OBS   */
/* frame (main-rendered callback) the display is composed into the next   */
/* slot; when the ring is full, the oldest slot is exactly N frames old   */
/* and is what the output shows. All on the GPU.                          */

/* Smooth delay changes: the shown frame's age (frames behind live) ramps
 * toward the target instead of jumping, so adjusting the delay live never
 * flashes black or jumps. More delay -> half-speed playback until reached;
 * less delay -> double speed. Turning delay on/off ramps from/to live. */
static constexpr double kRampSlower = 0.5; /* age gained per frame (plays at 0.5x) */
static constexpr double kRampFaster = 1.0; /* age lost per frame (plays at 2x)   */

struct DelayLine {
	uint32_t w = 0, h = 0;
	std::deque<gs_texrender_t *> hist;  /* oldest front, newest back */
	std::vector<gs_texrender_t *> pool; /* recycled render targets */
	double age = 0.0;                   /* frames behind live being shown */
	gs_texrender_t *show = nullptr;     /* what outputs draw this frame */
	bool failed = false;
};

static std::unordered_map<std::string, DelayLine> g_delays;

/* Identify overlay textures, per display (graphics thread only). */
struct IdentifyTex {
	qint64 key = 0;
	gs_texture_t *tex = nullptr;
};
static std::unordered_map<std::string, IdentifyTex> g_identify;
static std::unordered_map<std::string, IdentifyTex> g_identifyPreview;

static gs_texture_t *identifyTexture(const RenderDisplay &rd, bool preview = false)
{
	IdentifyTex &t = (preview ? g_identifyPreview : g_identify)[rd.id];
	const std::shared_ptr<const QImage> &img = preview ? rd.identifyPreviewImage : rd.identifyImage;
	const qint64 key = img->cacheKey();
	if (!t.tex || t.key != key) {
		gs_texture_destroy(t.tex);
		t.tex = createTextureFromImage(*img);
		t.key = key;
	}
	return t.tex;
}

static void destroyLine(DelayLine &l)
{
	for (gs_texrender_t *t : l.hist)
		gs_texrender_destroy(t);
	for (gs_texrender_t *t : l.pool)
		gs_texrender_destroy(t);
	l = DelayLine{};
}

static void delayTick(void *);

/* Dock thumbnails: requests/images shared with the UI (mutex), render
 * targets owned by the graphics thread. */
struct ThumbSlot {
	uint32_t w = 0, h = 0;
	gs_texrender_t *tr = nullptr;
	gs_stagesurf_t *stage[2] = {nullptr, nullptr};
	bool primed[2] = {false, false};
	int cycle = 0;
};
static std::mutex g_thumbMutex;
static std::vector<ThumbnailRequest> g_thumbReqs;
static std::unordered_map<std::string, QImage> g_thumbImages;
static std::unordered_map<std::string, ThumbSlot> g_thumbSlots;
static uint64_t g_thumbFrame = 0;

static void destroyThumb(ThumbSlot &t)
{
	gs_texrender_destroy(t.tr);
	gs_stagesurface_destroy(t.stage[0]);
	gs_stagesurface_destroy(t.stage[1]);
	t = ThumbSlot{};
}

static void thumbTick(const RenderState &st);

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

	obs_add_main_rendered_callback(delayTick, nullptr);

	gs_render_start(true);
	gs_vertex2f(0.0f, 0.0f);
	gs_vertex2f(1.0f, 0.0f);
	gs_vertex2f(0.0f, 1.0f);
	gs_vertex2f(1.0f, 1.0f);
	g_quad = gs_render_save();
}

void shutdown()
{
	obs_remove_main_rendered_callback(delayTick, nullptr);
	for (auto &kv : g_thumbSlots)
		destroyThumb(kv.second);
	g_thumbSlots.clear();
	for (auto &kv : g_delays)
		destroyLine(kv.second);
	g_delays.clear();
	for (auto *m : {&g_identify, &g_identifyPreview}) {
		for (auto &kv : *m)
			gs_texture_destroy(kv.second.tex);
		m->clear();
	}
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

void drawDisplayLive(const ProgramFrame &pf, const RenderDisplay &rd)
{
	/* Editor preview: test pattern without its baked-in slice outlines (the
	 * editor draws interactive ones); outlines mode shows live video and
	 * relies on the editor's own outlines. */
	if (rd.identify == 1 && rd.identifyPreviewImage && !rd.identifyPreviewImage->isNull()) {
		if (gs_texture_t *tex = identifyTexture(rd, true))
			drawTexture(tex, 0.0f, 0.0f, (float)rd.width, (float)rd.height);
		return;
	}
	drawSlices(pf, rd);
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

	auto line = g_delays.find(rd.id);
	if (rd.identify != 0 && rd.identifyImage && !rd.identifyImage->isNull()) {
		/* Identify mode: what's on this screen, right now (no delay). */
		if (rd.identify == 2) {
			ProgramFrame pf;
			if (acquireProgram(pf))
				drawSlices(pf, rd);
		}
		if (gs_texture_t *tex = identifyTexture(rd))
			drawTexture(tex, 0.0f, 0.0f, (float)rd.width, (float)rd.height);
	} else if (line != g_delays.end() && line->second.show) {
		/* Delayed (or ramping) output: draw the buffered frame. */
		gs_texture_t *tex = gs_texrender_get_texture(line->second.show);
		{
			if (tex) {
				const bool prevSrgb = gs_framebuffer_srgb_enabled();
				gs_enable_framebuffer_srgb(true);
				gs_blend_state_push();
				gs_enable_blending(false);
				gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
				gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
				gs_effect_set_texture_srgb(image, tex);
				gs_effect_set_float(gs_effect_get_param_by_name(effect, "multiplier"), 1.0f);
				gs_effect_set_next_sampler(image, g_point);
				while (gs_effect_loop(effect, "Draw"))
					gs_draw_sprite(tex, 0, (uint32_t)rd.width, (uint32_t)rd.height);
				gs_blend_state_pop();
				gs_enable_framebuffer_srgb(prevSrgb);
			}
		}
	} else {
		ProgramFrame pf;
		if (acquireProgram(pf))
			drawSlices(pf, rd);
	}

	gs_matrix_pop();
	gs_projection_pop();
	gs_viewport_pop();
}

double videoFps()
{
	obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi) || ovi.fps_den == 0)
		return 0.0;
	return (double)ovi.fps_num / (double)ovi.fps_den;
}

int delayFrames(int delayMs)
{
	if (delayMs <= 0)
		return 0;
	const double fps = videoFps();
	if (fps <= 0)
		return 0;
	return std::max(1, (int)std::lround(delayMs * fps / 1000.0));
}

static void delayTick(void *)
{
	auto st = RenderStore::instance().get();

	/* Lines for displays that no longer exist are dropped at once. */
	for (auto it = g_delays.begin(); it != g_delays.end();) {
		if (st->displays.find(it->first) == st->displays.end()) {
			destroyLine(it->second);
			it = g_delays.erase(it);
		} else {
			++it;
		}
	}

	/* Free identify textures that are no longer shown. */
	for (auto *m : {&g_identify, &g_identifyPreview}) {
		for (auto it = m->begin(); it != m->end();) {
			auto d = st->displays.find(it->first);
			if (d == st->displays.end() || d->second.identify == 0) {
				gs_texture_destroy(it->second.tex);
				it = m->erase(it);
			} else {
				++it;
			}
		}
	}

	ProgramFrame pf;
	const bool havePf = acquireProgram(pf);

	for (const auto &kv : st->displays) {
		const RenderDisplay &rd = kv.second;
		const int target = rd.delayMs > 0 ? delayFrames(rd.delayMs) : 0;
		auto existing = g_delays.find(kv.first);
		/* Live and not ramping: nothing to buffer. */
		if (target <= 0 && existing == g_delays.end())
			continue;
		if (rd.width <= 0 || rd.height <= 0)
			continue;

		DelayLine &l = g_delays[kv.first];
		const uint32_t w = (uint32_t)rd.width, h = (uint32_t)rd.height;
		if (l.w != w || l.h != h) {
			destroyLine(l); /* resolution changed: start over */
			l.w = w;
			l.h = h;
		}
		if (l.failed) {
			l.show = nullptr;
			continue;
		}

		/* Compose the current frame into a recycled (or new) target. */
		gs_texrender_t *tr = nullptr;
		if (!l.pool.empty()) {
			tr = l.pool.back();
			l.pool.pop_back();
		} else {
			tr = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
		}
		gs_texrender_reset(tr);
		if (!tr || !gs_texrender_begin_with_color_space(tr, w, h, GS_CS_SRGB)) {
			blog(LOG_ERROR, "[output-mapping] out of GPU memory for %ux%u delay buffer; delay disabled", w,
			     h);
			gs_texrender_destroy(tr);
			l.failed = true;
			l.show = nullptr;
			continue;
		}
		vec4 black;
		vec4_set(&black, 0.0f, 0.0f, 0.0f, 1.0f);
		gs_clear(GS_CLEAR_COLOR, &black, 0.0f, 0);
		gs_projection_push();
		gs_matrix_push();
		gs_matrix_identity();
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
		if (havePf)
			drawSlices(pf, rd);
		gs_matrix_pop();
		gs_projection_pop();
		gs_texrender_end(tr);
		l.hist.push_back(tr);

		/* Ramp the shown age toward the target. */
		if (l.age < target)
			l.age = std::min((double)target, l.age + kRampSlower);
		else if (l.age > target)
			l.age = std::max((double)target, l.age - kRampFaster);
		l.age = std::min(l.age, (double)(l.hist.size() - 1));

		/* Keep exactly the history still needed; recycle the rest. */
		const size_t keep = (size_t)std::max(target, (int)std::ceil(l.age)) + 1;
		while (l.hist.size() > keep) {
			l.pool.push_back(l.hist.front());
			l.hist.pop_front();
		}
		while (l.pool.size() > 2) {
			gs_texrender_destroy(l.pool.back());
			l.pool.pop_back();
		}

		if (target == 0 && l.age <= 0.0) {
			/* Ramped all the way back to live: hand over to direct
			 * compositing (identical picture) and free the buffer. */
			destroyLine(l);
			g_delays.erase(kv.first);
			continue;
		}
		const size_t idx = l.hist.size() - 1 - (size_t)std::floor(l.age);
		l.show = l.hist[idx];
	}

	thumbTick(*st);
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

void setThumbnailRequests(std::vector<ThumbnailRequest> requests)
{
	std::lock_guard<std::mutex> lock(g_thumbMutex);
	g_thumbReqs = std::move(requests);
	for (auto it = g_thumbImages.begin(); it != g_thumbImages.end();) {
		bool wanted = false;
		for (const auto &r : g_thumbReqs)
			wanted = wanted || r.displayId == it->first;
		it = wanted ? std::next(it) : g_thumbImages.erase(it);
	}
}

QImage latestThumbnail(const std::string &displayId)
{
	std::lock_guard<std::mutex> lock(g_thumbMutex);
	auto it = g_thumbImages.find(displayId);
	return it == g_thumbImages.end() ? QImage() : it->second;
}

static void thumbTick(const RenderState &st)
{
	/* ~10 fps whatever the OBS frame rate. */
	const double fps = videoFps();
	const uint64_t every = (uint64_t)std::max(1.0, std::round((fps > 0 ? fps : 30.0) / 10.0));
	if (g_thumbFrame++ % every != 0)
		return;

	std::vector<ThumbnailRequest> reqs;
	{
		std::lock_guard<std::mutex> lock(g_thumbMutex);
		reqs = g_thumbReqs;
	}
	for (auto it = g_thumbSlots.begin(); it != g_thumbSlots.end();) {
		bool wanted = false;
		for (const auto &r : reqs)
			wanted = wanted || r.displayId == it->first;
		if (!wanted) {
			destroyThumb(it->second);
			it = g_thumbSlots.erase(it);
		} else {
			++it;
		}
	}

	for (const ThumbnailRequest &r : reqs) {
		auto d = st.displays.find(r.displayId);
		const uint32_t w = std::clamp<uint32_t>(r.width, 16, 1280), h = std::clamp<uint32_t>(r.height, 9, 1280);
		if (d == st.displays.end())
			continue;
		ThumbSlot &t = g_thumbSlots[r.displayId];
		if (t.w != w || t.h != h) {
			destroyThumb(t);
			t.w = w;
			t.h = h;
			t.tr = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
			t.stage[0] = gs_stagesurface_create(w, h, GS_BGRA);
			t.stage[1] = gs_stagesurface_create(w, h, GS_BGRA);
		}
		if (!t.tr || !t.stage[0] || !t.stage[1])
			continue;

		gs_texrender_reset(t.tr);
		if (gs_texrender_begin_with_color_space(t.tr, w, h, GS_CS_SRGB)) {
			vec4 black;
			vec4_set(&black, 0.0f, 0.0f, 0.0f, 1.0f);
			gs_clear(GS_CLEAR_COLOR, &black, 0.0f, 0);
			renderDisplay(d->second, w, h, true); /* same path as the output */
			gs_texrender_end(t.tr);
		}
		const int k = t.cycle % 2, prev = (t.cycle + 1) % 2;
		gs_stage_texture(t.stage[k], gs_texrender_get_texture(t.tr));
		t.primed[k] = true;
		/* Read back the one staged a cycle ago: already finished on the GPU,
		 * so this never stalls the render loop. */
		if (t.primed[prev]) {
			uint8_t *data = nullptr;
			uint32_t linesize = 0;
			if (gs_stagesurface_map(t.stage[prev], &data, &linesize)) {
				QImage img((int)w, (int)h, QImage::Format_RGB32);
				for (uint32_t y = 0; y < h; y++)
					memcpy(img.scanLine((int)y), data + (size_t)y * linesize, (size_t)w * 4);
				gs_stagesurface_unmap(t.stage[prev]);
				std::lock_guard<std::mutex> lock(g_thumbMutex);
				g_thumbImages[r.displayId] = std::move(img);
			}
		}
		t.cycle++;
	}
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
