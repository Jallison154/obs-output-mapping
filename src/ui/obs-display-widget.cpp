/*
Output Mapping for OBS Studio
Copyright (C) 2026 Okami Designs

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "ui/obs-display-widget.hpp"

#include "platform/platform.hpp"

#include <QPlatformSurfaceEvent>
#include <QScreen>
#include <QWindow>

#if !defined(_WIN32) && !defined(__APPLE__)
#include <obs-nix-platform.h>
#endif

namespace om {

static bool toGsWindow(QWindow *window, gs_window &gw)
{
#ifdef _WIN32
	gw.hwnd = reinterpret_cast<void *>(window->winId());
	return true;
#elif defined(__APPLE__)
	gw.view = (id)window->winId();
	return true;
#else
	switch (obs_get_nix_platform()) {
	case OBS_NIX_PLATFORM_X11_EGL:
		gw.id = window->winId();
		gw.display = obs_get_nix_platform_display();
		return true;
#ifdef ENABLE_WAYLAND
	case OBS_NIX_PLATFORM_WAYLAND:
		gw.display = (void *)window->winId();
		return gw.display != nullptr;
#endif
	default:
		return false;
	}
#endif
}

ObsDisplayWidget::ObsDisplayWidget(QWidget *parent, Qt::WindowFlags flags) : QWidget(parent, flags)
{
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_StaticContents);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_DontCreateNativeAncestors);
	setAttribute(Qt::WA_NativeWindow);
	setMouseTracking(true);

	resizeTimer_ = new QTimer(this);
	resizeTimer_->setSingleShot(true);
	resizeTimer_->setInterval(30);
	connect(resizeTimer_, &QTimer::timeout, this, &ObsDisplayWidget::applyPendingResize);

	winId(); /* force native window creation so windowHandle() exists */
	if (QWindow *w = windowHandle()) {
		w->installEventFilter(this);
		connect(w, &QWindow::visibleChanged, this, [this](bool visible) {
			if (!visible)
				return;
			if (!display_)
				createDisplay();
			else
				scheduleResize();
		});
		connect(w, &QWindow::screenChanged, this, [this](QScreen *) {
			createDisplay();
			scheduleResize();
		});
	}
}

ObsDisplayWidget::~ObsDisplayWidget()
{
	destroying_ = true;
	destroyDisplay();
}

std::atomic<int> ObsDisplayWidget::surfaceResizes{0};

void ObsDisplayWidget::scheduleResize()
{
	if (display_ && !destroying_)
		resizeTimer_->start(); /* restarts: fires once the size settles */
}

void ObsDisplayWidget::applyPendingResize()
{
	if (!display_ || destroying_ || !isVisible())
		return;
	const QSize s = pixelSize();
	if (s.width() < 2 || s.height() < 2 || s == appliedSize_)
		return; /* degenerate or unchanged: keep the current swapchain */
	appliedSize_ = s;
	surfaceResizes++;
	obs_display_resize(display_, (uint32_t)s.width(), (uint32_t)s.height());
}

void ObsDisplayWidget::destroyDisplay()
{
	if (resizeTimer_)
		resizeTimer_->stop();
	appliedSize_ = QSize();
	if (display_) {
		obs_display_remove_draw_callback(display_, drawCallback, this);
		obs_display_destroy(display_); /* blocks until not rendering */
		display_ = nullptr;
	}
}

QSize ObsDisplayWidget::pixelSize() const
{
	/* Windows: the HWND client rect, exact even at fractional scaling
	 * (125 %, 175 % ...) where size()*dpr can be off by one pixel. */
	return nativePixelSize(const_cast<ObsDisplayWidget *>(this));
}

void ObsDisplayWidget::createDisplay()
{
	if (display_ || destroying_)
		return;
	QWindow *w = windowHandle();
	if (!w || !w->isExposed())
		return;

	QSize s = pixelSize();
	if (s.width() < 2 || s.height() < 2)
		return; /* not laid out yet: create once it has a real size */
	appliedSize_ = s;
	gs_init_data info = {};
	info.cx = (uint32_t)std::max(1, s.width());
	info.cy = (uint32_t)std::max(1, s.height());
	info.format = GS_BGRA;
	info.zsformat = GS_ZS_NONE;
	if (!toGsWindow(w, info.window))
		return;

	display_ = obs_display_create(&info, background_);
	if (display_)
		obs_display_add_draw_callback(display_, drawCallback, this);
}

void ObsDisplayWidget::drawCallback(void *param, uint32_t cx, uint32_t cy)
{
	static_cast<ObsDisplayWidget *>(param)->renderFrame(cx, cy);
}

void ObsDisplayWidget::paintEvent(QPaintEvent *e)
{
	createDisplay();
	QWidget::paintEvent(e);
}

void ObsDisplayWidget::showEvent(QShowEvent *e)
{
	QWidget::showEvent(e);
	createDisplay();
}

void ObsDisplayWidget::resizeEvent(QResizeEvent *e)
{
	QWidget::resizeEvent(e);
	createDisplay();
	scheduleResize();
}

QPaintEngine *ObsDisplayWidget::paintEngine() const
{
	return nullptr;
}

bool ObsDisplayWidget::eventFilter(QObject *obj, QEvent *e)
{
	if (e->type() == QEvent::PlatformSurface) {
		auto *se = static_cast<QPlatformSurfaceEvent *>(e);
		if (se->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
			destroyDisplay();
	} else if (e->type() == QEvent::Expose) {
		createDisplay();
	}
	return QWidget::eventFilter(obj, e);
}

} // namespace om
