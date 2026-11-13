/*
Output Mapping for OBS Studio
Copyright (C) 2026 Okami Designs

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Shared status colours / badges (editor sidebar and OBS dock).

#pragma once

#include "controller.hpp"
#include "ui/theme.hpp"

#include <QColor>
#include <QMessageBox>
#include <QPushButton>
#include <QString>

namespace om {

inline QColor statusColor(const DisplayStatus &st, DisplayKind kind)
{
	if (kind == DisplayKind::Virtual)
		return QColor(theme::kMuted);
	switch (st.match) {
	case MatchStatus::Connected:
	case MatchStatus::ConnectedSerial:
		return QColor(theme::kOk);
	case MatchStatus::Ambiguous:
		return QColor(theme::kWarn);
	case MatchStatus::Disconnected:
		return QColor(theme::kLive);
	default:
		return QColor(theme::kMuted);
	}
}

/* Short badge for the sidebar's second column. */
inline QString statusBadge(const DisplayStatus &st, DisplayKind kind, OutputMode m, QColor &color)
{
	if (m == OutputMode::Fullscreen) {
		color = QColor(theme::kLive);
		return QStringLiteral("● LIVE");
	}
	if (m == OutputMode::Window) {
		color = QColor(theme::kAccent);
		return QStringLiteral("WINDOW");
	}
	if (kind == DisplayKind::Virtual) {
		color = QColor(theme::kMuted);
		return QStringLiteral("VIRTUAL");
	}
	switch (st.match) {
	case MatchStatus::Ambiguous:
		color = QColor(theme::kWarn);
		return QStringLiteral("CHECK");
	case MatchStatus::Disconnected:
		color = QColor(theme::kLive);
		return QStringLiteral("OFFLINE");
	case MatchStatus::Unassigned:
		color = QColor(theme::kMuted);
		return QStringLiteral("UNSET");
	default:
		color = QColor(theme::kOk);
		return QStringLiteral("READY");
	}
}

/* Toggle the test pattern on every display. Asks first when outputs are
 * live. Returns true if the state changed. */
inline bool toggleTestPatternAll(QWidget *parent)
{
	Controller *c = Controller::instance();
	if (c->identifyCount() > 0) {
		c->clearIdentify();
		return true;
	}
	if (c->doc().displays.empty())
		return false;
	const int live = c->liveFullscreenCount();
	if (live > 0) {
		QMessageBox box(QMessageBox::Warning, QStringLiteral("Test Pattern on All Displays?"),
				QStringLiteral("%1 display(s) are live. The test pattern will appear on every display, "
					       "in front of the audience, until you clear it.")
					.arg(live),
				QMessageBox::NoButton, parent);
		QPushButton *show = box.addButton(QStringLiteral("Show on All Outputs"), QMessageBox::AcceptRole);
		box.addButton(QMessageBox::Cancel);
		box.setDefaultButton(QMessageBox::Cancel);
		box.exec();
		if (box.clickedButton() != show)
			return false;
	}
	c->setIdentifyAll(Identify::Pattern);
	return true;
}

} // namespace om
