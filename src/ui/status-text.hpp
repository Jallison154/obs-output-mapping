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

} // namespace om
