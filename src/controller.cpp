/*
Output Mapping for OBS Studio
Copyright (C) 2026 Okami Designs

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "controller.hpp"

#include "render/compositor.hpp"
#include "ui/output-window.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QApplication>
#include <QDateTime>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QScreen>

namespace om {

static Controller *g_controller = nullptr;

Controller *Controller::instance()
{
	return g_controller;
}

void Controller::create()
{
	if (!g_controller)
		g_controller = new Controller();
}

void Controller::destroy()
{
	delete g_controller;
	g_controller = nullptr;
}

Controller::Controller(QObject *parent) : QObject(parent)
{
	saveTimer_.setSingleShot(true);
	saveTimer_.setInterval(400);
	connect(&saveTimer_, &QTimer::timeout, this, &Controller::save);

	screenTimer_.setSingleShot(true);
	screenTimer_.setInterval(300);
	connect(&screenTimer_, &QTimer::timeout, this, &Controller::refreshScreens);

	programTimer_.setInterval(500);
	connect(&programTimer_, &QTimer::timeout, this, [this]() {
		uint32_t w = 0, h = 0;
		gfx::programSize(w, h);
		if ((int)w != programW_ || (int)h != programH_) {
			programW_ = (int)w;
			programH_ = (int)h;
			emit programSizeChanged();
		}
	});
	programTimer_.start();
	uint32_t w = 0, h = 0;
	gfx::programSize(w, h);
	programW_ = (int)w;
	programH_ = (int)h;

	auto *app = qGuiApp;
	connect(app, &QGuiApplication::screenAdded, this, [this](QScreen *s) {
		connect(s, &QScreen::geometryChanged, &screenTimer_, qOverload<>(&QTimer::start));
		screenTimer_.start();
	});
	connect(app, &QGuiApplication::screenRemoved, this, &Controller::onScreenRemoved);
	for (QScreen *s : QGuiApplication::screens())
		connect(s, &QScreen::geometryChanged, &screenTimer_, qOverload<>(&QTimer::start));

	watchdog_.setInterval(250);
	connect(&watchdog_, &QTimer::timeout, this, &Controller::verifyOutputs);
	installDisplayChangeHook([this]() {
		/* Synchronous: runs before the OS/Qt shuffle windows around. */
		verifyOutputs();
		screenTimer_.start();
	});

	refreshScreens();
}

Controller::~Controller()
{
	shutdown();
}

void Controller::shutdown()
{
	if (shuttingDown_)
		return;
	shuttingDown_ = true;
	if (loaded_)
		save();
	for (auto it = outputs_.begin(); it != outputs_.end(); ++it) {
		if (it.value()) {
			it.value()->closingFromCode_ = true;
			delete it.value().data();
		}
	}
	outputs_.clear();
	watchdog_.stop();
	removeDisplayChangeHook();
	setDisplaySleepInhibited(false);
}

/* ---------------------------------------------------------------------- */
/* Change propagation                                                      */

void Controller::changed(Change kind)
{
	/* Undo: record the state before this edit. Rapid geometry edits (a
	 * drag, holding an arrow key) collapse into a single step. */
	if (loaded_) {
		const qint64 now = QDateTime::currentMSecsSinceEpoch();
		const bool coalesce = kind == Change::Geometry && now - lastGeometryEdit_ < 700;
		if (!coalesce) {
			undo_.push_back(committed_);
			if (undo_.size() > 200)
				undo_.erase(undo_.begin());
		}
		lastGeometryEdit_ = kind == Change::Geometry ? now : 0;
		redo_.clear();
		committed_ = doc_;
		emit undoStateChanged();
	}
	if (!currentPreset_.isEmpty() && !presetModified_) {
		presetModified_ = true;
		emit presetsChanged();
	}
	RenderStore::instance().publish(buildRenderState(doc_));
	if (kind == Change::Structure)
		recomputeStatuses();
	if (loaded_)
		saveTimer_.start();
	emit documentChanged(kind);
}

void Controller::select(const QString &displayId, const QString &sliceId)
{
	QString d = displayId, s = sliceId;
	if (!s.isEmpty()) {
		if (DisplayMapping *dm = doc_.displayForSlice(s))
			d = dm->id;
		else
			s.clear();
	}
	if (!d.isEmpty() && doc_.displayIndex(d) < 0)
		d.clear();
	if (d == selDisplay_ && s == selSlice_)
		return;
	selDisplay_ = d;
	selSlice_ = s;
	emit selectionChanged();
}

void Controller::selectSlice(const QString &sliceId)
{
	select(QString(), sliceId);
}

/* ---------------------------------------------------------------------- */
/* Screens & status                                                        */

void Controller::rescanScreens()
{
	screens_ = enumerateScreens();
	recomputeStatuses();
	verifyOutputs();
	emit screensChanged();
}

void Controller::refreshScreens()
{
	rescanScreens();
	resumeWantedOutputs();
}

void Controller::recomputeStatuses()
{
	std::vector<DisplayIdentity> ids;
	ids.reserve(screens_.size());
	for (const auto &s : screens_)
		ids.push_back(s.identity);

	bool geometryUpdated = false;
	statuses_.clear();
	for (auto &d : doc_.displays) {
		DisplayStatus st;
		if (d.kind == DisplayKind::Virtual) {
			st.match = MatchStatus::Unassigned;
			st.text = QStringLiteral("Virtual · window only");
			statuses_.insert(d.id, st);
			continue;
		}
		MatchResult m = matchDisplay(d.identity, ids);
		/* A user confirmation (this session) upgrades an ambiguous match
		 * when that exact OS screen is still among the candidates. */
		if (m.status == MatchStatus::Ambiguous && sessionConfirmed_.contains(d.id)) {
			const QString nid = sessionConfirmed_.value(d.id);
			for (int c : m.candidates)
				if (screens_[(size_t)c].nativeId == nid) {
					m.status = MatchStatus::Connected;
					m.index = c;
				}
			if (m.status != MatchStatus::Connected)
				sessionConfirmed_.remove(d.id);
		} else if (m.status != MatchStatus::Ambiguous) {
			sessionConfirmed_.remove(d.id);
		}
		st.match = m.status;
		st.screen = m.index;
		st.candidates = m.candidates;
		switch (m.status) {
		case MatchStatus::Connected:
		case MatchStatus::ConnectedSerial: {
			const ScreenInfo &si = screens_[(size_t)m.index];
			st.exactPixels = si.exactPixels();
			st.canFullscreen = si.screen != nullptr;
			st.text = QStringLiteral("Connected · %1").arg(si.name);
			if (m.status == MatchStatus::ConnectedSerial)
				st.text += QStringLiteral(" (re-identified by serial)");
			/* Track the display's live resolution and refreshed identity. */
			if (d.width != si.pixelW || d.height != si.pixelH) {
				d.width = si.pixelW;
				d.height = si.pixelH;
				geometryUpdated = true;
			}
			if (d.identity.uuid != si.identity.uuid || d.identity.name != si.identity.name) {
				d.identity = si.identity;
				geometryUpdated = true;
			}
			break;
		}
		case MatchStatus::Ambiguous:
			st.text = QStringLiteral("Ambiguous identity · reassign");
			break;
		case MatchStatus::Disconnected:
			st.text = QStringLiteral("Disconnected");
			break;
		case MatchStatus::Unassigned:
			st.text = QStringLiteral("No display assigned");
			break;
		}
		statuses_.insert(d.id, st);
	}

	/* Two mappings resolving to the same physical display: only the first
	 * (sidebar order) may go fullscreen. */
	QHash<int, QString> owner;
	for (const auto &d : doc_.displays) {
		DisplayStatus &st = statuses_[d.id];
		if (!isConnected(st.match))
			continue;
		if (owner.contains(st.screen)) {
			st.canFullscreen = false;
			st.text += QStringLiteral(" · shared with another mapping");
		} else {
			owner.insert(st.screen, d.id);
		}
	}

	if (geometryUpdated) {
		RenderStore::instance().publish(buildRenderState(doc_));
		if (loaded_)
			saveTimer_.start();
	}
}

DisplayStatus Controller::status(const QString &displayId) const
{
	return statuses_.value(displayId);
}

void Controller::onScreenRemoved(QScreen *s)
{
	/* Close fullscreen outputs on the vanished display *immediately*,
	 * before the window system migrates them onto another screen. The
	 * mapping keeps outputWanted so it resumes on exact reconnection. */
	QStringList toClose;
	for (auto it = outputs_.begin(); it != outputs_.end(); ++it) {
		OutputWindow *w = it.value();
		if (w && w->mode() == OutputWindow::Mode::Fullscreen && (w->targetScreen() == s || !w->targetScreen()))
			toClose << it.key();
	}
	for (const QString &id : toClose) {
		blog(LOG_WARNING, "[output-mapping] display for '%s' disconnected; stopping its fullscreen output",
		     qPrintable(id));
		stopOutput(id, false);
	}
	screenTimer_.start();
}

/* ---------------------------------------------------------------------- */
/* Display mutations                                                       */

QString Controller::addPhysicalDisplay(int screenIndex)
{
	if (screenIndex < 0 || screenIndex >= (int)screens_.size())
		return QString();
	const ScreenInfo &si = screens_[(size_t)screenIndex];
	DisplayMapping d;
	d.id = newId();
	d.name = si.name;
	d.kind = DisplayKind::Physical;
	d.identity = si.identity;
	d.width = si.pixelW;
	d.height = si.pixelH;
	sessionConfirmed_.insert(d.id, si.nativeId);
	doc_.displays.push_back(d);
	changed(Change::Structure);
	select(d.id, QString());
	return d.id;
}

QString Controller::addVirtualDisplay(const QString &name, int w, int h)
{
	DisplayMapping d;
	d.id = newId();
	d.name = name.isEmpty() ? QStringLiteral("Virtual %1×%2").arg(w).arg(h) : name;
	d.kind = DisplayKind::Virtual;
	d.width = std::max(16, w);
	d.height = std::max(16, h);
	doc_.displays.push_back(d);
	changed(Change::Structure);
	select(d.id, QString());
	return d.id;
}

void Controller::removeDisplay(const QString &id)
{
	int i = doc_.displayIndex(id);
	if (i < 0)
		return;
	stopOutput(id, false);
	doc_.displays.erase(doc_.displays.begin() + i);
	if (selDisplay_ == id)
		select(doc_.displays.empty() ? QString() : doc_.displays.front().id, QString());
	changed(Change::Structure);
}

void Controller::renameDisplay(const QString &id, const QString &name)
{
	if (DisplayMapping *d = doc_.display(id)) {
		if (name.trimmed().isEmpty() || d->name == name)
			return;
		d->name = name.trimmed();
		if (outputs_.value(id))
			outputs_.value(id)->setWindowTitle(QStringLiteral("Output Mapping – %1").arg(d->name));
		changed(Change::Structure);
	}
}

void Controller::assignScreen(const QString &displayId, int screenIndex)
{
	DisplayMapping *d = doc_.display(displayId);
	if (!d)
		return;
	/* Changing the physical target always stops a running fullscreen. */
	if (outputMode(displayId) == OutputMode::Fullscreen)
		stopOutput(displayId, false);
	if (screenIndex < 0) {
		d->kind = DisplayKind::Virtual;
		d->identity = DisplayIdentity{};
		d->outputWanted = false;
	} else if (screenIndex < (int)screens_.size()) {
		const ScreenInfo &si = screens_[(size_t)screenIndex];
		d->kind = DisplayKind::Physical;
		d->identity = si.identity;
		d->width = si.pixelW;
		d->height = si.pixelH;
		/* An explicit pick is a confirmation for this session (matters only
		 * for displays without a hardware identity). */
		sessionConfirmed_.insert(d->id, si.nativeId);
	}
	changed(Change::Structure);
}

void Controller::setVirtualSize(const QString &id, int w, int h)
{
	DisplayMapping *d = doc_.display(id);
	if (!d || d->kind != DisplayKind::Virtual)
		return;
	w = std::max(16, w);
	h = std::max(16, h);
	if (d->width == w && d->height == h)
		return;
	d->width = w;
	d->height = h;
	changed(Change::Geometry);
}

void Controller::setDisplayDelay(const QString &id, int delayMs)
{
	DisplayMapping *d = doc_.display(id);
	delayMs = std::clamp(delayMs, 0, kMaxDelayMs);
	if (!d || d->delayMs == delayMs)
		return;
	d->delayMs = delayMs;
	blog(LOG_INFO, "[output-mapping] '%s' delay set to %d ms", qPrintable(d->name), delayMs);
	changed(Change::Structure); /* sidebar shows the delay badge */
}

void Controller::moveDisplay(const QString &id, int newIndex)
{
	int i = doc_.displayIndex(id);
	if (i < 0)
		return;
	newIndex = std::clamp(newIndex, 0, (int)doc_.displays.size() - 1);
	if (newIndex == i)
		return;
	DisplayMapping d = doc_.displays[(size_t)i];
	doc_.displays.erase(doc_.displays.begin() + i);
	doc_.displays.insert(doc_.displays.begin() + newIndex, d);
	changed(Change::Structure);
}

/* ---------------------------------------------------------------------- */
/* Slice mutations                                                         */

static int totalSlices(const MappingDocument &doc)
{
	int n = 0;
	for (const auto &d : doc.displays)
		n += (int)d.slices.size();
	return n;
}

QString Controller::createSlice(const QString &displayId, const IRect &src)
{
	DisplayMapping *d = doc_.display(displayId);
	if (!d || src.empty())
		return QString();
	Slice s;
	s.id = newId();
	int n = totalSlices(doc_);
	s.name = QStringLiteral("Slice %1").arg(n + 1);
	s.color = sliceColorForIndex(n);
	s.src = src;
	s.dst = defaultDestination(src, d->width, d->height);
	d->slices.insert(d->slices.begin(), s); /* new slices go on top */
	changed(Change::Structure);
	select(d->id, s.id);
	return s.id;
}

QString Controller::duplicateSlice(const QString &sliceId)
{
	DisplayMapping *d = doc_.displayForSlice(sliceId);
	if (!d)
		return QString();
	int i = d->sliceIndex(sliceId);
	Slice s = d->slices[(size_t)i];
	s.id = newId();
	s.locked = false; /* the copy is meant to be adjusted */
	s.name = s.name + QStringLiteral(" copy");
	s.color = sliceColorForIndex(totalSlices(doc_));
	s.dst.x += 20;
	s.dst.y += 20;
	d->slices.insert(d->slices.begin() + i, s);
	changed(Change::Structure);
	select(d->id, s.id);
	return s.id;
}

bool Controller::isSliceLocked(const QString &sliceId) const
{
	const Slice *s = doc_.slice(sliceId);
	return s && s->locked;
}

void Controller::setSliceLocked(const QString &sliceId, bool locked)
{
	Slice *s = doc_.slice(sliceId);
	if (!s || s->locked == locked)
		return;
	s->locked = locked;
	changed(Change::Structure);
}

void Controller::setDisplayLocked(const QString &displayId, bool locked)
{
	DisplayMapping *d = doc_.display(displayId);
	if (!d)
		return;
	bool any = false;
	for (Slice &s : d->slices)
		if (s.locked != locked) {
			s.locked = locked;
			any = true;
		}
	if (any)
		changed(Change::Structure);
}

void Controller::deleteSlice(const QString &sliceId)
{
	DisplayMapping *d = doc_.displayForSlice(sliceId);
	if (!d || isSliceLocked(sliceId))
		return; /* locked slices can't be deleted */
	int i = d->sliceIndex(sliceId);
	d->slices.erase(d->slices.begin() + i);
	QString next;
	if (!d->slices.empty())
		next = d->slices[(size_t)std::min(i, (int)d->slices.size() - 1)].id;
	QString did = d->id;
	changed(Change::Structure);
	select(did, next);
}

void Controller::renameSlice(const QString &sliceId, const QString &name)
{
	Slice *s = doc_.slice(sliceId);
	if (!s || name.trimmed().isEmpty() || s->name == name.trimmed())
		return;
	s->name = name.trimmed();
	changed(Change::Structure);
}

void Controller::setSliceEnabled(const QString &sliceId, bool enabled)
{
	Slice *s = doc_.slice(sliceId);
	if (!s || s->enabled == enabled)
		return;
	s->enabled = enabled;
	changed(Change::Structure);
}

void Controller::setSliceSrc(const QString &sliceId, const IRect &r)
{
	Slice *s = doc_.slice(sliceId);
	if (!s || s->locked || r.empty() || s->src == r)
		return;
	s->src = r;
	changed(Change::Geometry);
}

void Controller::setSliceDst(const QString &sliceId, const IRect &r)
{
	Slice *s = doc_.slice(sliceId);
	if (!s || s->locked || r.empty() || s->dst == r)
		return;
	s->dst = r;
	changed(Change::Geometry);
}

void Controller::moveSlice(const QString &sliceId, const QString &toDisplayId, int index)
{
	DisplayMapping *from = doc_.displayForSlice(sliceId);
	DisplayMapping *to = doc_.display(toDisplayId);
	if (!from || !to)
		return;
	int i = from->sliceIndex(sliceId);
	Slice s = from->slices[(size_t)i];
	if (from != to && s.locked)
		return; /* locked: may be reordered, not moved to another display */
	if (from == to) {
		index = std::clamp(index, 0, (int)to->slices.size() - 1);
		if (index == i)
			return;
		from->slices.erase(from->slices.begin() + i);
		to->slices.insert(to->slices.begin() + index, s);
	} else {
		from->slices.erase(from->slices.begin() + i);
		index = std::clamp(index, 0, (int)to->slices.size());
		/* Keep the destination on the new display. */
		if (s.dst.right() > to->width || s.dst.bottom() > to->height) {
			IRect fit = defaultDestination(s.dst, to->width, to->height);
			s.dst.w = fit.w;
			s.dst.h = fit.h;
			s.dst.x = std::clamp(s.dst.x, 0, std::max(0, to->width - s.dst.w));
			s.dst.y = std::clamp(s.dst.y, 0, std::max(0, to->height - s.dst.h));
		}
		to->slices.insert(to->slices.begin() + index, s);
	}
	QString tid = to->id;
	changed(Change::Structure);
	select(tid, sliceId);
}

void Controller::fitSliceToDisplay(const QString &sliceId)
{
	DisplayMapping *d = doc_.displayForSlice(sliceId);
	Slice *s = doc_.slice(sliceId);
	if (!d || !s)
		return;
	setSliceDst(sliceId, fitToDisplay(s->src, d->width, d->height));
}

void Controller::sliceOneToOne(const QString &sliceId)
{
	Slice *s = doc_.slice(sliceId);
	if (!s)
		return;
	setSliceDst(sliceId, {s->dst.x, s->dst.y, s->src.w, s->src.h});
}

void Controller::replaceDocument(const MappingDocument &doc)
{
	stopAll();
	doc_ = doc;
	selDisplay_.clear();
	selSlice_.clear();
	changed(Change::Structure);
	if (!doc_.displays.empty())
		select(doc_.displays.front().id,
		       doc_.displays.front().slices.empty() ? QString() : doc_.displays.front().slices.front().id);
	else
		emit selectionChanged();
}

void Controller::restoreSnapshot(const MappingDocument &d)
{
	/* Keep live state that isn't an "edit": run flags and live sizes. */
	MappingDocument next = d;
	for (auto &nd : next.displays)
		if (const DisplayMapping *cur = doc_.display(nd.id))
			nd.outputWanted = cur->outputWanted;
	next.restoreOutputs = doc_.restoreOutputs;
	next.excludeFromCapture = doc_.excludeFromCapture;
	doc_ = next;
	committed_ = doc_;
	lastGeometryEdit_ = 0;
	/* Outputs of displays that no longer exist must stop. */
	for (const QString &id : outputs_.keys())
		if (!doc_.display(id))
			stopOutput(id, false);
	RenderStore::instance().publish(buildRenderState(doc_));
	recomputeStatuses();
	saveTimer_.start();
	emit documentChanged(Change::Structure);
	QString d0 = selDisplay_, s0 = selSlice_;
	selDisplay_.clear();
	selSlice_.clear();
	if (doc_.slice(s0))
		select(QString(), s0);
	else if (doc_.display(d0))
		select(d0, QString());
	else if (!doc_.displays.empty())
		select(doc_.displays.front().id, QString());
	else
		emit selectionChanged();
	emit undoStateChanged();
}

void Controller::undo()
{
	if (undo_.empty())
		return;
	redo_.push_back(doc_);
	MappingDocument d = undo_.back();
	undo_.pop_back();
	restoreSnapshot(d);
}

void Controller::redo()
{
	if (redo_.empty())
		return;
	undo_.push_back(doc_);
	MappingDocument d = redo_.back();
	redo_.pop_back();
	restoreSnapshot(d);
}

/* ---------------------------------------------------------------------- */
/* Layout presets & files                                                  */

static bool parseLayout(const QByteArray &raw, MappingDocument &out, QString *error)
{
	QJsonParseError perr{};
	QJsonDocument jd = QJsonDocument::fromJson(raw, &perr);
	if (perr.error != QJsonParseError::NoError || !jd.isObject()) {
		if (error)
			*error = QStringLiteral("Not a valid layout file (%1).").arg(perr.errorString());
		return false;
	}
	QJsonObject o = jd.object();
	if (!o.value("displays").isArray()) {
		if (error)
			*error = QStringLiteral("Not an Output Mapping layout (no \"displays\").");
		return false;
	}
	if (o.value("version").toInt(1) > 1) {
		if (error)
			*error = QStringLiteral("This layout was saved by a newer version of Output Mapping.");
		return false;
	}
	out = MappingDocument::fromJson(o);
	return true;
}

bool Controller::applyLayout(const MappingDocument &incoming, const QString &presetName)
{
	MappingDocument next = incoming;
	/* App-level options are not part of a layout. */
	next.restoreOutputs = doc_.restoreOutputs;
	next.excludeFromCapture = doc_.excludeFromCapture;

	/* Keep outputs whose display survives unchanged; stop the rest. */
	for (const QString &id : outputs_.keys()) {
		const DisplayMapping *cur = doc_.display(id);
		DisplayMapping *nd = next.display(id);
		const bool same = cur && nd && cur->kind == nd->kind && cur->identity.uuid == nd->identity.uuid;
		if (!same)
			stopOutput(id, false);
	}
	for (auto &nd : next.displays) {
		const DisplayMapping *cur = doc_.display(nd.id);
		/* Run state follows what is actually running, never the file. */
		nd.outputWanted = cur && outputs_.value(nd.id) && cur->outputWanted;
	}

	doc_ = next;
	currentPreset_.clear(); /* so changed() doesn't flag it modified */
	changed(Change::Structure);
	currentPreset_ = presetName;
	presetModified_ = false;
	QString keepD = selDisplay_, keepS = selSlice_;
	selDisplay_.clear();
	selSlice_.clear();
	if (doc_.slice(keepS))
		select(QString(), keepS);
	else if (doc_.display(keepD))
		select(keepD, QString());
	else if (!doc_.displays.empty())
		select(doc_.displays.front().id, QString());
	else
		emit selectionChanged();
	emit presetsChanged();
	blog(LOG_INFO, "[output-mapping] applied layout%s%s (%d display(s))", presetName.isEmpty() ? "" : " ",
	     qPrintable(presetName), (int)doc_.displays.size());
	return true;
}

QString Controller::presetsDir() const
{
	char *p = obs_module_config_path("layouts");
	QString out = QString::fromUtf8(p);
	bfree(p);
	return out;
}

QString Controller::presetPath(const QString &name) const
{
	QString safe = name.trimmed();
	safe.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));
	return presetsDir() + "/" + safe + ".json";
}

QStringList Controller::presetNames() const
{
	QStringList names;
	QDir dir(presetsDir());
	for (const QFileInfo &fi :
	     dir.entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name | QDir::IgnoreCase))
		names << fi.completeBaseName();
	return names;
}

bool Controller::savePreset(const QString &name, QString *error)
{
	if (name.trimmed().isEmpty()) {
		if (error)
			*error = QStringLiteral("Enter a name.");
		return false;
	}
	QDir().mkpath(presetsDir());
	if (!exportLayout(presetPath(name), error))
		return false;
	currentPreset_ = QFileInfo(presetPath(name)).completeBaseName();
	presetModified_ = false;
	emit presetsChanged();
	return true;
}

bool Controller::loadPreset(const QString &name, QString *error)
{
	QFile f(presetPath(name));
	if (!f.open(QIODevice::ReadOnly)) {
		if (error)
			*error = QStringLiteral("Preset “%1” not found.").arg(name);
		return false;
	}
	MappingDocument d;
	if (!parseLayout(f.readAll(), d, error))
		return false;
	return applyLayout(d, QFileInfo(f.fileName()).completeBaseName());
}

bool Controller::deletePreset(const QString &name, QString *error)
{
	if (!QFile::remove(presetPath(name))) {
		if (error)
			*error = QStringLiteral("Could not delete preset “%1”.").arg(name);
		return false;
	}
	if (currentPreset_ == name)
		currentPreset_.clear();
	emit presetsChanged();
	return true;
}

bool Controller::exportLayout(const QString &path, QString *error) const
{
	QJsonObject o = doc_.toJson();
	/* Run state and app options are not part of a layout. */
	o.remove("restoreOutputs");
	o.remove("excludeFromCapture");
	QJsonArray ds = o.value("displays").toArray();
	for (int i = 0; i < ds.size(); i++) {
		QJsonObject d = ds[i].toObject();
		d.remove("outputWanted");
		ds[i] = d;
	}
	o["displays"] = ds;
	QSaveFile f(path);
	if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(o).toJson(QJsonDocument::Indented)) < 0 ||
	    !f.commit()) {
		if (error)
			*error = QStringLiteral("Could not write %1.").arg(path);
		return false;
	}
	return true;
}

bool Controller::importLayout(const QString &path, QString *error)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly)) {
		if (error)
			*error = QStringLiteral("Could not open %1.").arg(path);
		return false;
	}
	MappingDocument d;
	if (!parseLayout(f.readAll(), d, error))
		return false;
	return applyLayout(d, QString());
}

void Controller::setRestoreOutputs(bool on)
{
	doc_.restoreOutputs = on;
	if (loaded_)
		saveTimer_.start();
}

/* ---------------------------------------------------------------------- */
/* Outputs                                                                 */

OutputMode Controller::outputMode(const QString &displayId) const
{
	OutputWindow *w = outputs_.value(displayId);
	if (!w)
		return OutputMode::None;
	return w->mode() == OutputWindow::Mode::Fullscreen ? OutputMode::Fullscreen : OutputMode::Window;
}

bool Controller::startFullscreen(const QString &displayId, QString *error)
{
	auto fail = [&](const QString &msg) {
		if (error)
			*error = msg;
		blog(LOG_WARNING, "[output-mapping] fullscreen refused for '%s': %s", qPrintable(displayId),
		     qPrintable(msg));
		return false;
	};

	DisplayMapping *d = doc_.display(displayId);
	if (!d)
		return fail(QStringLiteral("Unknown display."));
	if (d->kind == DisplayKind::Virtual)
		return fail(QStringLiteral("Virtual displays can only be opened as a window."));

	/* Re-check identity against the live display list right now (rescan
	 * only: resuming from here would recurse back into startFullscreen). */
	rescanScreens();
	DisplayStatus st = status(displayId);
	if (!isConnected(st.match))
		return fail(QStringLiteral("Display is %1. Reassign it before starting fullscreen.")
				    .arg(QString::fromLatin1(matchStatusName(st.match))));
	if (!st.canFullscreen)
		return fail(QStringLiteral("Another mapping already owns this display."));
	const ScreenInfo &si = screens_[(size_t)st.screen];
	if (!si.screen)
		return fail(QStringLiteral("Qt has no screen for this display."));

	for (auto it = outputs_.begin(); it != outputs_.end(); ++it) {
		OutputWindow *w = it.value();
		if (w && it.key() != displayId && w->mode() == OutputWindow::Mode::Fullscreen &&
		    w->targetScreen() == si.screen)
			return fail(QStringLiteral("Another fullscreen output is already on this display."));
	}

	if (outputs_.value(displayId))
		stopOutput(displayId, false);

	auto *w = new OutputWindow(displayId, OutputWindow::Mode::Fullscreen, si.screen,
				   QStringLiteral("Output Mapping – %1").arg(d->name), d->width, d->height);
	connect(w, &OutputWindow::closeRequested, this, [this](const QString &id) { stopOutput(id, true); });
	outputs_.insert(displayId, w);
	w->showOutput(optionsFor(si.screen));
	watchdog_.start();

	d->outputWanted = true;
	saveTimer_.start();
	setDisplaySleepInhibited(true);
	blog(LOG_INFO, "[output-mapping] fullscreen '%s' -> %s (%dx%d px, backing %dx%d)", qPrintable(d->name),
	     qPrintable(si.name), si.pixelW, si.pixelH, si.backingW, si.backingH);
	emit outputsChanged();
	return true;
}

bool Controller::openWindow(const QString &displayId, QString *error)
{
	DisplayMapping *d = doc_.display(displayId);
	if (!d) {
		if (error)
			*error = QStringLiteral("Unknown display.");
		return false;
	}
	if (outputs_.value(displayId))
		stopOutput(displayId, false);

	auto *w = new OutputWindow(
		displayId, OutputWindow::Mode::Window, nullptr,
		QStringLiteral("Output Mapping – %1 (%2×%3)").arg(d->name).arg(d->width).arg(d->height), d->width,
		d->height);
	connect(w, &OutputWindow::closeRequested, this, [this](const QString &id) { stopOutput(id, true); });
	outputs_.insert(displayId, w);
	w->showOutput(optionsFor(nullptr));
	emit outputsChanged();
	return true;
}

void Controller::stopOutput(const QString &displayId, bool userInitiated)
{
	QPointer<OutputWindow> w = outputs_.take(displayId);
	if (w) {
		bool wasFullscreen = w->mode() == OutputWindow::Mode::Fullscreen;
		w->closingFromCode_ = true;
		w->hide();
		delete w.data();
		if (wasFullscreen && userInitiated) {
			if (DisplayMapping *d = doc_.display(displayId)) {
				d->outputWanted = false;
				saveTimer_.start();
			}
		}
	}
	bool anyFullscreen = false;
	for (auto &o : outputs_)
		if (o && o->mode() == OutputWindow::Mode::Fullscreen)
			anyFullscreen = true;
	setDisplaySleepInhibited(anyFullscreen);
	if (!anyFullscreen)
		watchdog_.stop();
	if (!shuttingDown_)
		emit outputsChanged();
}

void Controller::stopAll()
{
	const QStringList ids = outputs_.keys();
	for (const QString &id : ids)
		stopOutput(id, true);
}

void Controller::startAll()
{
	for (const auto &d : doc_.displays) {
		if (d.kind != DisplayKind::Physical || outputMode(d.id) == OutputMode::Fullscreen)
			continue;
		DisplayStatus st = status(d.id);
		if (isConnected(st.match) && st.canFullscreen)
			startFullscreen(d.id);
	}
}

OutputWindowOptions Controller::optionsFor(QScreen *target) const
{
	OutputWindowOptions o;
	o.excludeFromCapture = doc_.excludeFromCapture;
	/* Allow focus (and Esc) only if the output covers the OBS window. */
	auto *main = static_cast<QWidget *>(obs_frontend_get_main_window());
	o.allowFocus = target && main && main->screen() == target;
	return o;
}

void Controller::verifyOutputs()
{
	if (shuttingDown_)
		return;
	QStringList toStop;
	for (auto it = outputs_.begin(); it != outputs_.end(); ++it) {
		OutputWindow *w = it.value();
		if (!w || w->mode() != OutputWindow::Mode::Fullscreen)
			continue;
		QScreen *target = w->targetScreen();
		const bool screenGone = !target || !QGuiApplication::screens().contains(target);
		/* The mapping must still resolve to the same physical display. */
		DisplayStatus st = status(it.key());
		const bool identityMoved = !isConnected(st.match) || st.screen < 0 ||
					   st.screen >= (int)screens_.size() ||
					   screens_[(size_t)st.screen].screen != target;
		if (screenGone || identityMoved) {
			blog(LOG_WARNING, "[output-mapping] output '%s' lost its display (%s); stopping it",
			     qPrintable(it.key()), screenGone ? "display removed" : "identity changed");
			toStop << it.key();
			continue;
		}
		if (outputWindowOnTarget(w, target)) {
			w->misplacedChecks = 0;
			continue;
		}
		/* Moved or resized by the OS (resolution change, taskbar, DPI): put
		 * it back. If that keeps failing, hide it rather than show video in
		 * the wrong place. */
		if (++w->misplacedChecks > 8) {
			blog(LOG_WARNING, "[output-mapping] output '%s' cannot be kept on its display; stopping it",
			     qPrintable(it.key()));
			toStop << it.key();
		} else {
			w->reapplyPlacement();
		}
	}
	for (const QString &id : toStop)
		stopOutput(id, false); /* keeps outputWanted: resumes on exact reconnection */
}

void Controller::setExcludeFromCapture(bool on)
{
	if (doc_.excludeFromCapture == on)
		return;
	doc_.excludeFromCapture = on;
	for (auto &w : outputs_)
		if (w && w->mode() == OutputWindow::Mode::Fullscreen)
			w->showOutput(optionsFor(w->targetScreen()));
	if (loaded_)
		saveTimer_.start();
}

void Controller::resumeWantedOutputs()
{
	if (!loaded_ || shuttingDown_ || !doc_.restoreOutputs || resuming_)
		return;
	resuming_ = true;
	QStringList toStart;
	for (const auto &d : doc_.displays) {
		if (!d.outputWanted || d.kind != DisplayKind::Physical || outputMode(d.id) != OutputMode::None)
			continue;
		DisplayStatus st = status(d.id);
		if (isConnected(st.match) && st.canFullscreen)
			toStart << d.id;
	}
	for (const QString &id : toStart) {
		if (const DisplayMapping *d = doc_.display(id))
			blog(LOG_INFO, "[output-mapping] resuming fullscreen output '%s'", qPrintable(d->name));
		startFullscreen(id);
	}
	resuming_ = false;
}

void Controller::restoreOutputsOnStartup()
{
	for (const auto &d : doc_.displays) {
		if (!d.outputWanted)
			continue;
		DisplayStatus st = status(d.id);
		if (!isConnected(st.match))
			blog(LOG_WARNING, "[output-mapping] not restoring '%s': display %s", qPrintable(d.name),
			     matchStatusName(st.match));
	}
	resumeWantedOutputs();
}

QImage Controller::snapshot(const QString &displayId, QString *error)
{
	RenderDisplay rd;
	if (!RenderStore::instance().getDisplay(displayId.toStdString(), rd) || rd.width <= 0 || rd.height <= 0) {
		if (error)
			*error = QStringLiteral("Unknown display.");
		return QImage();
	}
	/* Same renderDisplay() path as the fullscreen output. */
	return gfx::captureOffscreen((uint32_t)rd.width, (uint32_t)rd.height,
				     [rd](uint32_t cx, uint32_t cy) { gfx::renderDisplay(rd, cx, cy, false); }, error);
}

/* ---------------------------------------------------------------------- */
/* Persistence                                                             */

QString Controller::configPath() const
{
	char *p = obs_module_config_path("mappings.json");
	QString out = QString::fromUtf8(p);
	bfree(p);
	return out;
}

void Controller::load()
{
	QString path = configPath();
	QFile f(path);
	if (f.open(QIODevice::ReadOnly)) {
		QJsonParseError err{};
		QJsonDocument jd;
		const QByteArray raw = f.readAll();
		f.close();
		jd = QJsonDocument::fromJson(raw, &err);
		if (err.error == QJsonParseError::NoError && jd.isObject()) {
			doc_ = MappingDocument::fromJson(jd.object());
			blog(LOG_INFO, "[output-mapping] loaded %d display mapping(s) from %s",
			     (int)doc_.displays.size(), qPrintable(path));
			QFile::remove(path + ".bak");
			QFile::copy(path, path + ".bak"); /* last known-good copy */
		} else {
			/* Never overwrite a file we could not read: set it aside. */
			QString aside =
				path + ".unreadable-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
			QFile::rename(path, aside);
			blog(LOG_ERROR, "[output-mapping] could not parse %s (%s); moved it to %s", qPrintable(path),
			     qPrintable(err.errorString()), qPrintable(aside));
		}
	}
	rescanScreens();
	committed_ = doc_;
	RenderStore::instance().publish(buildRenderState(doc_));
	emit documentChanged(Change::Structure);
	loaded_ = true; /* outputs are restored later by restoreOutputsOnStartup() */
	if (!doc_.displays.empty())
		select(doc_.displays.front().id, QString());
}

void Controller::save()
{
	if (!loaded_)
		return;
	char *dir = obs_module_config_path("");
	os_mkdirs(dir);
	bfree(dir);
	QSaveFile f(configPath());
	if (f.open(QIODevice::WriteOnly)) {
		f.write(QJsonDocument(doc_.toJson()).toJson(QJsonDocument::Indented));
		f.commit();
	}
}

QJsonObject Controller::stateJson() const
{
	QJsonObject o = doc_.toJson();
	QJsonArray ds = o.value("displays").toArray();
	for (int i = 0; i < ds.size(); i++) {
		QJsonObject d = ds[i].toObject();
		QString id = d.value("id").toString();
		DisplayStatus st = status(id);
		d["status"] = QString::fromLatin1(matchStatusName(st.match));
		d["statusText"] = st.text;
		d["canFullscreen"] = st.canFullscreen;
		OutputMode m = outputMode(id);
		d["output"] = m == OutputMode::Fullscreen ? "fullscreen" : m == OutputMode::Window ? "window" : "none";
		ds[i] = d;
	}
	o["displays"] = ds;
	QJsonArray ss;
	for (const auto &s : screens_) {
		ss.append(QJsonObject{
			{"name", s.name},
			{"uuid", QString::fromStdString(s.identity.uuid)},
			{"vendor", (qint64)s.identity.vendor},
			{"model", (qint64)s.identity.model},
			{"serial", (qint64)s.identity.serial},
			{"pixelWidth", s.pixelW},
			{"pixelHeight", s.pixelH},
			{"backingWidth", s.backingW},
			{"backingHeight", s.backingH},
			{"x", s.geometry.x()},
			{"y", s.geometry.y()},
			{"builtin", s.builtin},
			{"mirrored", s.mirrored},
			{"nativeId", s.nativeId},
		});
	}
	o["screens"] = ss;
	o["programWidth"] = programW_;
	o["programHeight"] = programH_;
	o["selectedDisplay"] = selDisplay_;
	o["selectedSlice"] = selSlice_;
	return o;
}

} // namespace om
