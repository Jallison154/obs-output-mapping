/*
Output Mapping for OBS Studio
Copyright (C) 2026 Okami Designs

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Output Mapping's own look: graphite surfaces, amber accent, signal-red
// for live outputs. Scoped to the editor window only.

#pragma once

#include <QString>

namespace om::theme {

inline const char *kAccent = "#FFA62B";
inline const char *kLive = "#FF4D5E";
inline const char *kOk = "#3DDC97";
inline const char *kWarn = "#FFC542";
inline const char *kMuted = "#8C94A3";

inline QString styleSheet()
{
	return QStringLiteral(R"(
QDialog#OutputMappingEditor { background: #101217; }
QWidget { color: #DDE1E8; font-size: 12px; }
QToolBar { background: #15181E; border: none; border-bottom: 1px solid #232731; padding: 4px 6px; spacing: 4px; }
QToolBar QToolButton { background: transparent; border: 1px solid transparent; border-radius: 5px; padding: 4px 9px; color: #DDE1E8; }
QToolBar QToolButton:hover { background: #222631; border-color: #2E3340; }
QToolBar QToolButton:checked { background: #2A2416; border-color: #FFA62B; color: #FFC77A; }
QToolBar QToolButton:disabled { color: #545B68; }
QToolButton#MenuButton::menu-indicator { image: none; width: 0; }
QToolBar::separator { background: #2A2E38; width: 1px; margin: 4px 6px; }
QSplitter::handle { background: #101217; }
QSplitter::handle:horizontal { width: 6px; }
QSplitter::handle:vertical { height: 6px; }
QFrame#Panel { background: #15181E; border: 1px solid #232731; border-radius: 8px; }
QLabel#PanelTitle { font-size: 11px; font-weight: 700; letter-spacing: 1.5px; color: #FFA62B; }
QLabel#PanelSub { color: #8C94A3; }
QLabel#Zoom { color: #8C94A3; font-family: Menlo, Consolas, "Cascadia Mono", monospace; font-size: 11px; }
QLabel#Chip { background: #2A2416; color: #FFC77A; border: 1px solid #5A4520; border-radius: 9px; padding: 1px 8px; font-size: 11px; }
QFrame#EmptyState { background: #15181E; border: 1px dashed #2E3340; border-radius: 8px; }
QLabel#EmptyTitle { font-size: 15px; font-weight: 600; color: #DDE1E8; }
QLabel#SectionHint { color: #8C94A3; font-size: 11px; }
QTreeWidget { background: #15181E; border: 1px solid #232731; border-radius: 8px; padding: 4px; outline: none; }
QTreeWidget::item { height: 24px; border-radius: 4px; padding-left: 2px; }
QTreeWidget::item:hover { background: #1E222B; }
QTreeWidget::item:selected { background: #2A2416; color: #FFE2B8; }
QGroupBox { background: #15181E; border: 1px solid #232731; border-radius: 8px; margin-top: 18px; padding: 10px 8px 8px 8px; }
QGroupBox::title { subcontrol-origin: margin; left: 10px; top: 2px; color: #8C94A3; font-size: 10px; font-weight: 700; letter-spacing: 1.2px; }
QLineEdit, QSpinBox, QComboBox { background: #0E1014; border: 1px solid #2A2E38; border-radius: 4px; padding: 3px 5px; selection-background-color: #FFA62B; selection-color: #101217; }
QLineEdit:focus, QSpinBox:focus, QComboBox:focus { border-color: #FFA62B; }
QSpinBox:disabled, QLineEdit:disabled, QComboBox:disabled { color: #5F6675; background: #13151A; border-color: #1F232B; }
QSpinBox { font-family: Menlo, Consolas, "Cascadia Mono", monospace; font-size: 11px; }
QComboBox QAbstractItemView { background: #15181E; border: 1px solid #2A2E38; selection-background-color: #2A2416; }
QPushButton { background: #222631; border: 1px solid #2E3340; border-radius: 5px; padding: 5px 10px; }
QPushButton:hover { background: #2A2F3B; }
QPushButton:disabled { color: #545B68; background: #181B21; }
QPushButton#Primary { background: #FFA62B; color: #15120A; border-color: #FFA62B; font-weight: 600; }
QPushButton#Primary:hover { background: #FFB54F; }
QPushButton#Primary:disabled { background: #2A2416; color: #7A6A50; border-color: #3A3020; }
QPushButton#Live { background: #FF4D5E; color: white; border-color: #FF4D5E; font-weight: 600; }
QPushButton#Danger { color: #FF8A95; }
QCheckBox::indicator { width: 14px; height: 14px; }
QStatusBar, QWidget#StatusBar { background: #15181E; border-top: 1px solid #232731; }
QLabel#Status { color: #8C94A3; font-family: Menlo, Consolas, "Cascadia Mono", monospace; font-size: 11px; padding: 3px 8px; }
QMenu { background: #181B21; border: 1px solid #2A2E38; padding: 4px; }
QMenu::item { padding: 5px 18px; border-radius: 4px; }
QMenu::item:selected { background: #2A2416; }
QMenu::item:disabled { color: #545B68; }
QToolTip { background: #222631; color: #DDE1E8; border: 1px solid #2E3340; }
)");
}

} // namespace om::theme
