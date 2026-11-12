/*
Output Mapping for OBS Studio
Copyright (C) 2026 Okami Designs

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "controller.hpp"
#include "render/compositor.hpp"
#include "ui/mapping-editor.hpp"
#include "ui/preview-view.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <cassert>

#include "thirdparty/obs-websocket-api.h"

#include <QAction>
#include <QApplication>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QThread>
#include <QMainWindow>
#include <QScreen>
#include <QPointer>
#include <QTimer>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

using namespace om;

static QPointer<MappingEditor> g_editor;
static obs_hotkey_id g_hkStart = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id g_hkStop = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id g_hkEditor = OBS_INVALID_HOTKEY_ID;
static obs_websocket_vendor g_vendor = nullptr;
static bool g_exiting = false;

static MappingEditor *openEditor()
{
	if (!Controller::instance())
		return nullptr;
	if (!g_editor) {
		auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
		g_editor = new MappingEditor(main);
		/* First open: fit and centre on the screen showing OBS. */
		QScreen *scr = main ? main->screen() : QGuiApplication::primaryScreen();
		if (scr) {
			QRect avail = scr->availableGeometry();
			QSize sz = g_editor->size().boundedTo(avail.size() * 0.92);
			g_editor->resize(sz);
			g_editor->move(avail.center() - QPoint(sz.width() / 2, sz.height() / 2));
		}
	}
	g_editor->show();
	g_editor->raise();
	g_editor->activateWindow();
	return g_editor;
}

/* ---------------------------------------------------------------------- */
/* Hotkeys (persisted next to the mappings)                                */

static QString hotkeyPath()
{
	char *p = obs_module_config_path("hotkeys.json");
	QString s = QString::fromUtf8(p);
	bfree(p);
	return s;
}

static void loadHotkeys()
{
	obs_data_t *d = obs_data_create_from_json_file(hotkeyPath().toUtf8().constData());
	if (!d)
		return;
	auto load = [&](obs_hotkey_id id, const char *key) {
		obs_data_array_t *a = obs_data_get_array(d, key);
		obs_hotkey_load(id, a);
		obs_data_array_release(a);
	};
	load(g_hkStart, "start_all");
	load(g_hkStop, "stop_all");
	load(g_hkEditor, "open_editor");
	obs_data_release(d);
}

static void saveHotkeys()
{
	obs_data_t *d = obs_data_create();
	auto save = [&](obs_hotkey_id id, const char *key) {
		obs_data_array_t *a = obs_hotkey_save(id);
		obs_data_set_array(d, key, a);
		obs_data_array_release(a);
	};
	save(g_hkStart, "start_all");
	save(g_hkStop, "stop_all");
	save(g_hkEditor, "open_editor");
	char *dir = obs_module_config_path("");
	os_mkdirs(dir);
	bfree(dir);
	obs_data_save_json_safe(d, hotkeyPath().toUtf8().constData(), "tmp", "bak");
	obs_data_release(d);
}

static void onUiThread(std::function<void()> fn)
{
	QMetaObject::invokeMethod(qApp, std::move(fn), Qt::QueuedConnection);
}

static void registerHotkeys()
{
	g_hkStart = obs_hotkey_register_frontend(
		"output_mapping.start_all", "Output Mapping: Start all fullscreen outputs",
		[](void *, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
			if (pressed)
				onUiThread([]() {
					if (Controller::instance())
						Controller::instance()->startAll();
				});
		},
		nullptr);
	g_hkStop = obs_hotkey_register_frontend(
		"output_mapping.stop_all", "Output Mapping: Stop all outputs",
		[](void *, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
			if (pressed)
				onUiThread([]() {
					if (Controller::instance())
						Controller::instance()->stopAll();
				});
		},
		nullptr);
	g_hkEditor = obs_hotkey_register_frontend(
		"output_mapping.open_editor", "Output Mapping: Open editor",
		[](void *, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
			if (pressed)
				onUiThread([]() { openEditor(); });
		},
		nullptr);
	loadHotkeys();
}

/* ---------------------------------------------------------------------- */
/* obs-websocket vendor requests ("output-mapping")                        */

static void runBlocking(std::function<void()> fn)
{
	if (QThread::currentThread() == qApp->thread())
		fn();
	else
		QMetaObject::invokeMethod(qApp, std::move(fn), Qt::BlockingQueuedConnection);
}

static QString resolveDisplay(obs_data_t *req)
{
	Controller *c = Controller::instance();
	QString id = QString::fromUtf8(obs_data_get_string(req, "displayId"));
	if (!id.isEmpty() && c->doc().display(id))
		return id;
	QString name = QString::fromUtf8(obs_data_get_string(req, "displayName"));
	for (const auto &d : c->doc().displays)
		if (d.name == name)
			return d.id;
	return QString();
}

static void setError(obs_data_t *res, const QString &err)
{
	obs_data_set_bool(res, "ok", false);
	obs_data_set_string(res, "error", err.toUtf8().constData());
}

static void setStateJson(obs_data_t *res)
{
	QByteArray json = QJsonDocument(Controller::instance()->stateJson()).toJson(QJsonDocument::Compact);
	obs_data_t *st = obs_data_create_from_json(json.constData());
	obs_data_set_obj(res, "state", st);
	obs_data_release(st);
	obs_data_set_bool(res, "ok", true);
}

static bool automationEnabled()
{
	const char *v = getenv("OUTPUT_MAPPING_AUTOMATION");
	return v && *v == '1';
}

#define VENDOR_REQ(name) static void req_##name(obs_data_t *req, obs_data_t *res, void *)

VENDOR_REQ(GetState)
{
	UNUSED_PARAMETER(req);
	runBlocking([&]() { setStateJson(res); });
}

VENDOR_REQ(SetState)
{
	runBlocking([&]() {
		obs_data_t *st = obs_data_get_obj(req, "state");
		if (!st) {
			setError(res, "missing 'state'");
			return;
		}
		QJsonDocument jd = QJsonDocument::fromJson(obs_data_get_json(st));
		obs_data_release(st);
		Controller::instance()->replaceDocument(MappingDocument::fromJson(jd.object()));
		setStateJson(res);
	});
}

VENDOR_REQ(StartFullscreen)
{
	runBlocking([&]() {
		QString id = resolveDisplay(req), err;
		if (id.isEmpty())
			return setError(res, "unknown display");
		if (!Controller::instance()->startFullscreen(id, &err))
			return setError(res, err);
		setStateJson(res);
	});
}

VENDOR_REQ(OpenWindow)
{
	runBlocking([&]() {
		QString id = resolveDisplay(req), err;
		if (id.isEmpty())
			return setError(res, "unknown display");
		if (!Controller::instance()->openWindow(id, &err))
			return setError(res, err);
		setStateJson(res);
	});
}

VENDOR_REQ(StopOutput)
{
	runBlocking([&]() {
		QString id = resolveDisplay(req);
		if (id.isEmpty())
			return setError(res, "unknown display");
		Controller::instance()->stopOutput(id, true);
		setStateJson(res);
	});
}

VENDOR_REQ(StopAll)
{
	UNUSED_PARAMETER(req);
	runBlocking([&]() {
		Controller::instance()->stopAll();
		setStateJson(res);
	});
}

VENDOR_REQ(StartAll)
{
	UNUSED_PARAMETER(req);
	runBlocking([&]() {
		Controller::instance()->startAll();
		setStateJson(res);
	});
}

VENDOR_REQ(Snapshot)
{
	/* Wait for the frame off the UI thread is unnecessary: snapshot()
	 * only blocks on the graphics thread, never on the UI thread. */
	QString id, path = QString::fromUtf8(obs_data_get_string(req, "path"));
	runBlocking([&]() { id = resolveDisplay(req); });
	if (id.isEmpty())
		return setError(res, "unknown display");
	QString err;
	QImage img = Controller::instance()->snapshot(id, &err);
	if (img.isNull())
		return setError(res, err);
	if (!path.isEmpty() && !img.save(path)) /* format from suffix: .png, .ppm, ... */
		return setError(res, "could not write " + path);
	obs_data_set_int(res, "width", img.width());
	obs_data_set_int(res, "height", img.height());
	obs_data_set_bool(res, "ok", true);
}

VENDOR_REQ(OpenEditor)
{
	runBlocking([&]() {
		MappingEditor *e = openEditor();
		int w = (int)obs_data_get_int(req, "width"), h = (int)obs_data_get_int(req, "height");
		if (e && w > 0 && h > 0)
			e->resize(w, h);
		obs_data_set_bool(res, "ok", e != nullptr);
	});
}

VENDOR_REQ(CloseEditor)
{
	UNUSED_PARAMETER(req);
	runBlocking([&]() {
		if (g_editor)
			g_editor->close();
		obs_data_set_bool(res, "ok", true);
	});
}

VENDOR_REQ(Select)
{
	runBlocking([&]() {
		QString display = resolveDisplay(req);
		QString slice = QString::fromUtf8(obs_data_get_string(req, "sliceId"));
		QString sliceName = QString::fromUtf8(obs_data_get_string(req, "sliceName"));
		if (slice.isEmpty() && !sliceName.isEmpty())
			for (const auto &d : Controller::instance()->doc().displays)
				for (const auto &s : d.slices)
					if (s.name == sliceName)
						slice = s.id;
		Controller::instance()->select(display, slice);
		setStateJson(res);
	});
}

VENDOR_REQ(Undo)
{
	UNUSED_PARAMETER(req);
	runBlocking([&]() {
		Controller::instance()->undo();
		setStateJson(res);
	});
}

VENDOR_REQ(Redo)
{
	UNUSED_PARAMETER(req);
	runBlocking([&]() {
		Controller::instance()->redo();
		setStateJson(res);
	});
}

VENDOR_REQ(EditorCapture)
{
	runBlocking([&]() {
		MappingEditor *e = openEditor();
		QString path = QString::fromUtf8(obs_data_get_string(req, "path"));
		QImage img = e ? e->captureImage() : QImage();
		if (img.isNull() || path.isEmpty() || !img.save(path))
			return setError(res, "capture failed");
		obs_data_set_int(res, "width", img.width());
		obs_data_set_int(res, "height", img.height());
		obs_data_set_bool(res, "ok", true);
	});
}

/* Automation only (OUTPUT_MAPPING_AUTOMATION=1): drive the real editor
 * mouse handlers with synthetic events, in canvas coordinates. */
VENDOR_REQ(EditorDrag)
{
	runBlocking([&]() {
		if (!automationEnabled())
			return setError(res, "set OUTPUT_MAPPING_AUTOMATION=1 to enable");
		MappingEditor *e = openEditor();
		if (!e)
			return setError(res, "no editor");
		PreviewView *v = QString::fromUtf8(obs_data_get_string(req, "view")) == "output" ? e->outputView()
												   : e->inputView();
		QPointF from(obs_data_get_double(req, "fromX"), obs_data_get_double(req, "fromY"));
		QPointF to(obs_data_get_double(req, "toX"), obs_data_get_double(req, "toY"));
		Qt::KeyboardModifiers mods;
		if (obs_data_get_bool(req, "shift"))
			mods |= Qt::ShiftModifier;
		if (obs_data_get_bool(req, "alt"))
			mods |= Qt::AltModifier;
		v->simulateDrag(from, to, mods);
		setStateJson(res);
	});
}

VENDOR_REQ(EditorKey)
{
	runBlocking([&]() {
		if (!automationEnabled())
			return setError(res, "set OUTPUT_MAPPING_AUTOMATION=1 to enable");
		MappingEditor *e = openEditor();
		PreviewView *v = QString::fromUtf8(obs_data_get_string(req, "view")) == "output" ? e->outputView()
												   : e->inputView();
		int key = (int)obs_data_get_int(req, "key");
		Qt::KeyboardModifiers mods = obs_data_get_bool(req, "shift") ? Qt::ShiftModifier : Qt::NoModifier;
		QKeyEvent press(QEvent::KeyPress, key, mods);
		QKeyEvent release(QEvent::KeyRelease, key, mods);
		QCoreApplication::sendEvent(v, &press);
		QCoreApplication::sendEvent(v, &release);
		setStateJson(res);
	});
}

static void registerVendor()
{
	g_vendor = obs_websocket_register_vendor("output-mapping");
	if (!g_vendor) {
		obs_log(LOG_INFO, "obs-websocket not available; vendor requests disabled");
		return;
	}
#define REG(name) obs_websocket_vendor_register_request(g_vendor, #name, req_##name, nullptr)
	REG(GetState);
	REG(SetState);
	REG(StartFullscreen);
	REG(OpenWindow);
	REG(StopOutput);
	REG(StopAll);
	REG(StartAll);
	REG(Snapshot);
	REG(OpenEditor);
	REG(CloseEditor);
	REG(Select);
	REG(Undo);
	REG(Redo);
	REG(EditorCapture);
	REG(EditorDrag);
	REG(EditorKey);
#undef REG
	obs_log(LOG_INFO, "registered obs-websocket vendor 'output-mapping'");
}

/* ---------------------------------------------------------------------- */
/* Lifecycle                                                               */

static void teardown()
{
	if (g_exiting)
		return;
	g_exiting = true;
	saveHotkeys();
	if (Controller::instance())
		Controller::instance()->shutdown(); /* saves, closes outputs */
	delete g_editor.data();
	Controller::destroy();
	obs_enter_graphics();
	gfx::shutdown();
	obs_leave_graphics();
}

static void frontendEvent(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		if (Controller::instance()) {
			Controller::instance()->load();
			/* Give the main window a moment, then restore outputs only
			 * onto displays whose identity matches exactly. */
			QTimer::singleShot(1500, qApp, []() {
				if (Controller::instance())
					Controller::instance()->restoreOutputsOnStartup();
			});
		}
		if (getenv("OUTPUT_MAPPING_OPEN_EDITOR"))
			QTimer::singleShot(500, qApp, []() { openEditor(); });
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		teardown();
		break;
	default:
		break;
	}
}

bool obs_module_load(void)
{
	obs_enter_graphics();
	gfx::init();
	obs_leave_graphics();

	Controller::create();
	registerHotkeys();

	auto *action = static_cast<QAction *>(obs_frontend_add_tools_menu_qaction("Output Mapping…"));
	QObject::connect(action, &QAction::triggered, []() { openEditor(); });
	auto *stopAll = static_cast<QAction *>(obs_frontend_add_tools_menu_qaction("Output Mapping: Stop All Outputs"));
	QObject::connect(stopAll, &QAction::triggered, []() {
		if (Controller::instance())
			Controller::instance()->stopAll();
	});

	obs_frontend_add_event_callback(frontendEvent, nullptr);
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_post_load(void)
{
	registerVendor();
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(frontendEvent, nullptr);
	teardown();
	obs_log(LOG_INFO, "plugin unloaded");
}

const char *obs_module_description(void)
{
	return "Output Mapping: slice the Program canvas onto multiple displays (Resolume-style advanced output).";
}
