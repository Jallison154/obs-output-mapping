/*
Output Mapping for OBS Studio
Copyright (C) 2026 Okami Designs

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "ui/output-dock.hpp"

#include "controller.hpp"
#include "render/compositor.hpp"
#include "ui/status-text.hpp"

#include <obs-frontend-api.h>

#include <QCheckBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

namespace om {

std::atomic<bool> DockPreview::live{true};

DockPreview::DockPreview(const QString &displayId, QWidget *parent)
	: ObsDisplayWidget(parent),
	  id_(displayId.toStdString())
{
	setBackground(0x101010);
	setMinimumSize(80, 45);
}

DockPreview::~DockPreview()
{
	destroyDisplay(); /* stop render callbacks before members die */
}

QImage DockPreview::capture()
{
	QSize px = pixelSize();
	QImage img = gfx::captureOffscreen((uint32_t)px.width(), (uint32_t)px.height(),
					   [this](uint32_t cx, uint32_t cy) { renderFrame(cx, cy); });
	img.setDevicePixelRatio(devicePixelRatioF());
	return img;
}

void DockPreview::renderFrame(uint32_t cx, uint32_t cy)
{
	if (!live.load())
		return;
	RenderDisplay rd;
	if (RenderStore::instance().getDisplay(id_, rd))
		gfx::renderDisplay(rd, cx, cy, true); /* same path as the real output */
}

/* A tile keeps its preview at the display's aspect ratio. */
class DockTile : public QFrame {
public:
	DockTile(DockPreview *preview, int dispW, int dispH, QWidget *parent)
		: QFrame(parent),
		  preview_(preview),
		  aspect_(dispW > 0 && dispH > 0 ? (double)dispH / dispW : 9.0 / 16.0)
	{
	}

protected:
	void resizeEvent(QResizeEvent *e) override
	{
		QFrame::resizeEvent(e);
		const int w = std::max(80, preview_->width());
		preview_->setFixedHeight(std::min(360, std::max(45, (int)(w * aspect_))));
	}

private:
	DockPreview *preview_;
	double aspect_;
};

OutputDock::OutputDock(std::function<void()> openEditor, QWidget *parent)
	: QWidget(parent),
	  openEditor_(std::move(openEditor))
{
	setObjectName("OutputMappingDock");
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(4, 4, 4, 4);
	root->setSpacing(4);

	auto *bar = new QHBoxLayout();
	auto *startAll = new QPushButton(QStringLiteral("▶ Start All"), this);
	startAll->setToolTip(QStringLiteral("Start fullscreen on every display that is connected and identified"));
	auto *stopAll = new QPushButton(QStringLiteral("■ Stop All"), this);
	stopAll->setToolTip(QStringLiteral("Stop every output (asks first if any are live)"));
	auto *editor = new QPushButton(QStringLiteral("Editor…"), this);
	editor->setToolTip(QStringLiteral("Open the Output Mapping editor"));
	auto *patternsAll = new QPushButton(this);
	patternsAll->setToolTip(QStringLiteral("Show the test pattern on every display (asks first if any are "
					       "live); click again to clear all"));
	auto updatePatternsAll = [patternsAll]() {
		const bool any = Controller::instance()->identifyCount() > 0;
		patternsAll->setText(any ? QStringLiteral("Clear All Patterns") : QStringLiteral("Test Pattern All"));
	};
	updatePatternsAll();
	connect(patternsAll, &QPushButton::clicked, this, [this]() { toggleTestPatternAll(this); });
	connect(Controller::instance(), &Controller::identifyChanged, patternsAll, updatePatternsAll);
	bar->addWidget(startAll);
	bar->addWidget(stopAll);
	bar->addWidget(patternsAll);
	bar->addStretch(1);
	bar->addWidget(editor);
	root->addLayout(bar);

	liveCheck_ = new QCheckBox(QStringLiteral("Live previews"), this);
	liveCheck_->setChecked(DockPreview::live.load());
	liveCheck_->setToolTip(QStringLiteral("Turn off to stop drawing the thumbnails (saves GPU); outputs are "
					      "not affected"));
	root->addWidget(liveCheck_);

	auto *scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	auto *container = new QWidget(scroll);
	tiles_ = new QVBoxLayout(container);
	tiles_->setContentsMargins(0, 0, 0, 0);
	tiles_->setSpacing(6);
	scroll->setWidget(container);
	root->addWidget(scroll, 1);

	connect(startAll, &QPushButton::clicked, this, []() { Controller::instance()->startAll(); });
	connect(stopAll, &QPushButton::clicked, this, &OutputDock::stopAllConfirmed);
	connect(editor, &QPushButton::clicked, this, [this]() { openEditor_(); });
	connect(liveCheck_, &QCheckBox::toggled, this, [](bool on) { DockPreview::live.store(on); });

	/* Rebuild (coalesced) whenever displays, outputs or status change. */
	auto *debounce = new QTimer(this);
	debounce->setSingleShot(true);
	debounce->setInterval(50);
	connect(debounce, &QTimer::timeout, this, &OutputDock::rebuild);
	Controller *c = Controller::instance();
	connect(c, &Controller::documentChanged, this, [debounce](Change k) {
		if (k == Change::Structure)
			debounce->start();
	});
	connect(c, &Controller::outputsChanged, debounce, qOverload<>(&QTimer::start));
	connect(c, &Controller::screensChanged, debounce, qOverload<>(&QTimer::start));
	connect(c, &Controller::identifyChanged, debounce, qOverload<>(&QTimer::start));
	rebuild();
}

QImage OutputDock::captureImage()
{
	QPixmap pm = grab();
	QPainter p(&pm);
	for (DockPreview *v : findChildren<DockPreview *>()) {
		QImage img = v->capture();
		if (!img.isNull())
			p.drawImage(QRect(v->mapTo(this, QPoint(0, 0)), v->size()), img);
	}
	p.end();
	return pm.toImage();
}

int OutputDock::tileCount() const
{
	return (int)findChildren<DockPreview *>().size();
}

void OutputDock::stopAllConfirmed()
{
	Controller *c = Controller::instance();
	int live = 0;
	for (const DisplayMapping &d : c->doc().displays)
		if (c->outputMode(d.id) == OutputMode::Fullscreen)
			live++;
	if (live > 0) {
		QMessageBox box(
			QMessageBox::Warning, QStringLiteral("Stop All Outputs?"),
			QStringLiteral("%1 fullscreen output(s) are live. Stopping them blacks out those screens.")
				.arg(live),
			QMessageBox::NoButton, this);
		QPushButton *stop = box.addButton(QStringLiteral("Stop All"), QMessageBox::DestructiveRole);
		box.addButton(QMessageBox::Cancel);
		box.setDefaultButton(QMessageBox::Cancel);
		box.exec();
		if (box.clickedButton() != stop)
			return;
	}
	c->stopAll();
}

QWidget *OutputDock::makeTile(const QString &id)
{
	Controller *c = Controller::instance();
	const DisplayMapping *d = c->doc().display(id);
	auto *preview = new DockPreview(id);
	auto *tile = new DockTile(preview, d->width, d->height, nullptr);
	tile->setFrameShape(QFrame::StyledPanel);
	auto *v = new QVBoxLayout(tile);
	v->setContentsMargins(6, 6, 6, 6);
	v->setSpacing(4);

	const DisplayStatus st = c->status(id);
	const OutputMode m = c->outputMode(id);
	QColor badgeColor;
	const QString badge = statusBadge(st, d->kind, m, badgeColor);

	auto *head = new QHBoxLayout();
	auto *name = new QLabel(QStringLiteral("<b>%1</b>&nbsp;&nbsp;<span style='opacity:0.7'>%2×%3</span>")
					.arg(d->name.toHtmlEscaped())
					.arg(d->width)
					.arg(d->height),
				tile);
	name->setToolTip(st.text);
	auto *badgeLbl = new QLabel(QStringLiteral("<b style='color:%1'>%2</b>").arg(badgeColor.name(), badge), tile);
	badgeLbl->setToolTip(st.text);
	head->addWidget(name, 1);
	head->addWidget(badgeLbl);
	v->addLayout(head);

	QStringList notes;
	if (d->delayMs > 0)
		notes << QStringLiteral("⏱ %1 ms delay").arg(d->delayMs);
	const Identify idm = c->identify(id);
	if (idm != Identify::Off)
		notes << QStringLiteral("<b style='color:%1'>⚠ %2 on output</b>")
				 .arg(theme::kLive)
				 .arg(idm == Identify::Pattern ? "test pattern" : "slice outlines");
	if (!notes.isEmpty()) {
		auto *n = new QLabel(notes.join(QStringLiteral(" · ")), tile);
		v->addWidget(n);
	}

	v->addWidget(preview);

	auto *buttons = new QHBoxLayout();
	auto *fs = new QPushButton(m == OutputMode::Fullscreen ? QStringLiteral("■ Stop Fullscreen")
							       : QStringLiteral("▶ Fullscreen"),
				   tile);
	fs->setEnabled(m == OutputMode::Fullscreen ||
		       (d->kind == DisplayKind::Physical && isConnected(st.match) && st.canFullscreen));
	fs->setToolTip(d->kind == DisplayKind::Virtual ? QStringLiteral("Virtual displays only open as a window")
						       : st.text);
	auto *win = new QPushButton(m == OutputMode::Window ? QStringLiteral("Close Window") : QStringLiteral("Window"),
				    tile);
	buttons->addWidget(fs, 1);
	buttons->addWidget(win);
	if (idm != Identify::Off) {
		auto *clear = new QPushButton(QStringLiteral("Clear Pattern"), tile);
		clear->setToolTip(QStringLiteral("Turn off the test pattern / outlines on this output"));
		connect(clear, &QPushButton::clicked, tile,
			[id]() { Controller::instance()->setIdentify(id, Identify::Off); });
		buttons->addWidget(clear);
	}
	v->addLayout(buttons);

	connect(fs, &QPushButton::clicked, tile, [this, id]() {
		Controller *ctl = Controller::instance();
		if (ctl->outputMode(id) == OutputMode::Fullscreen) {
			ctl->stopOutput(id);
			return;
		}
		const DisplayStatus s = ctl->status(id);
		if (isConnected(s.match) && s.screen >= 0 && s.screen < (int)ctl->screens().size()) {
			auto *mainWin = static_cast<QWidget *>(obs_frontend_get_main_window());
			if (mainWin && mainWin->screen() == ctl->screens()[(size_t)s.screen].screen &&
			    QMessageBox::question(this, QStringLiteral("Cover this screen?"),
						  QStringLiteral(
							  "This display is showing OBS. The output will cover "
							  "it.\n\nTo stop it: click the output and press Esc, or "
							  "use Stop All.")) != QMessageBox::Yes)
				return;
		}
		QString err;
		if (!ctl->startFullscreen(id, &err))
			QMessageBox::warning(this, QStringLiteral("Can't Start Fullscreen"), err);
	});
	connect(win, &QPushButton::clicked, tile, [id]() {
		Controller *ctl = Controller::instance();
		if (ctl->outputMode(id) == OutputMode::Window)
			ctl->stopOutput(id);
		else
			ctl->openWindow(id);
	});
	return tile;
}

void OutputDock::rebuild()
{
	/* Destroy old tiles (their obs_displays) before creating new ones. */
	while (QLayoutItem *it = tiles_->takeAt(0)) {
		if (QWidget *w = it->widget())
			delete w;
		delete it;
	}
	Controller *c = Controller::instance();
	if (!c)
		return;
	if (c->doc().displays.empty()) {
		auto *empty = new QLabel(QStringLiteral("No displays mapped yet.\nOpen the editor to add one."));
		empty->setAlignment(Qt::AlignCenter);
		empty->setWordWrap(true);
		tiles_->addWidget(empty);
	} else {
		for (const DisplayMapping &d : c->doc().displays)
			tiles_->addWidget(makeTile(d.id));
	}
	tiles_->addStretch(1);
}

} // namespace om
