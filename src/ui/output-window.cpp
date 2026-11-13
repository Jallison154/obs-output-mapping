/*
Output Mapping for OBS Studio
Copyright (C) 2026 Okami Designs

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "ui/output-window.hpp"

#include "platform/platform.hpp"
#include "render/compositor.hpp"

#include <QCloseEvent>
#include <QKeyEvent>
#include <QWindow>

namespace om {

static Qt::WindowFlags flagsFor(OutputWindow::Mode m)
{
	if (m == OutputWindow::Mode::Fullscreen)
		return Qt::Window | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint;
	return Qt::Window;
}

OutputWindow::OutputWindow(const QString &displayId, Mode mode, QScreen *screen, const QString &title, int dispW,
			   int dispH)
	: ObsDisplayWidget(nullptr, flagsFor(mode)),
	  displayId_(displayId.toStdString()),
	  mode_(mode),
	  screen_(screen),
	  dispW_(dispW),
	  dispH_(dispH)
{
	setAttribute(Qt::WA_DeleteOnClose, false);
	setWindowTitle(title);
	setObjectName(QStringLiteral("output:%1").arg(title));
	setBackground(0x000000);

	if (mode == Mode::Fullscreen) {
		setAttribute(Qt::WA_ShowWithoutActivating);
		setCursor(Qt::BlankCursor);
		if (screen && windowHandle())
			windowHandle()->setScreen(screen);
		if (screen)
			setGeometry(screen->geometry());
	} else {
		/* Windowed: open at 1:1 pixels if it fits, else 75% of the screen. */
		QScreen *s = screen ? screen : this->screen();
		double dpr = s ? s->devicePixelRatio() : 1.0;
		QSize want((int)(dispW / dpr), (int)(dispH / dpr));
		if (s) {
			QSize avail = s->availableGeometry().size() * 0.75;
			if (want.width() > avail.width() || want.height() > avail.height())
				want.scale(avail, Qt::KeepAspectRatio);
		}
		resize(want);
		setMinimumSize(160, 90);
	}
}

OutputWindow::~OutputWindow()
{
	destroyDisplay(); /* stop render callbacks before members die */
}

void OutputWindow::showOutput(const OutputWindowOptions &opts)
{
	allowFocus_ = opts.allowFocus;
	excludeFromCapture_ = opts.excludeFromCapture;
	if (mode_ == Mode::Fullscreen) {
		show();
		configureOutputWindow(this, screen_, opts);
	} else {
		show();
		raise();
	}
}

void OutputWindow::reapplyPlacement()
{
	if (mode_ != Mode::Fullscreen || !screen_)
		return;
	OutputWindowOptions o;
	o.allowFocus = allowFocus_;
	o.excludeFromCapture = excludeFromCapture_;
	setGeometry(screen_->geometry());
	configureOutputWindow(this, screen_, o);
}

void OutputWindow::renderFrame(uint32_t cx, uint32_t cy)
{
	frames_++;
	RenderDisplay rd;
	if (!RenderStore::instance().getDisplay(displayId_, rd))
		return;
	/* Fullscreen: viewport == display backing store, so this is 1:1 when
	 * the display runs at its native mode. Letterboxes if it does not. */
	gfx::renderDisplay(rd, cx, cy, true);
}

void OutputWindow::keyPressEvent(QKeyEvent *e)
{
	/* Esc stops windowed outputs, and fullscreen only when it covers the
	 * OBS window's own screen (so you can never get stuck). A stray key on
	 * a show output on another display does nothing. */
	if (e->key() == Qt::Key_Escape && (mode_ == Mode::Window || allowFocus_)) {
		emit closeRequested(QString::fromStdString(displayId_));
		return;
	}
	ObsDisplayWidget::keyPressEvent(e);
}

void OutputWindow::closeEvent(QCloseEvent *e)
{
	if (!closingFromCode_ && mode_ == Mode::Fullscreen) {
		/* Borderless outputs have no close button: an external close
		 * means the application is quitting. Close quietly and keep the
		 * mapping's restore flag so the output comes back next launch. */
		ObsDisplayWidget::closeEvent(e);
		return;
	}
	if (!closingFromCode_) {
		e->ignore();
		emit closeRequested(QString::fromStdString(displayId_));
		return;
	}
	ObsDisplayWidget::closeEvent(e);
}

} // namespace om
