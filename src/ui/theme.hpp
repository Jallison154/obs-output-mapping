/*
Output Mapping for OBS Studio
Copyright (C) 2026 Okami Designs

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Vexel's look (Okami Designs brand guide): deep navy surfaces, cyan accent,
// cyan -> purple gradient for primary actions, signal-red for live outputs.
// Scoped to the plugin's own windows only.

#pragma once

#include <QString>

namespace om::theme {

inline const char *kAccent = "#19D3F5";
inline const char *kLive = "#FF4D5E";
inline const char *kOk = "#3DDC97";
inline const char *kWarn = "#FFC542";
inline const char *kMuted = "#8A97AE";

inline QString styleSheet()
{
	return QStringLiteral(R"(
QDialog#OutputMappingEditor { background: #0A0F1C; }
QWidget { color: #DCE6F5; font-size: 12px; }
QToolBar { background: #0F1626; border: none; border-bottom: 1px solid #1C2740; padding: 4px 6px; spacing: 4px; }
QToolBar QToolButton { background: transparent; border: 1px solid transparent; border-radius: 5px; padding: 4px 9px; color: #DCE6F5; }
QToolBar QToolButton:hover { background: #172238; border-color: #26344F; }
QToolBar QToolButton:checked { background: #0E2A3A; border-color: #19D3F5; color: #7FE9FF; }
QToolBar QToolButton:disabled { color: #4E5A72; }
QToolButton#MenuButton::menu-indicator { image: none; width: 0; }
QToolBar::separator { background: #22304A; width: 1px; margin: 4px 6px; }
QSplitter::handle { background: #0A0F1C; }
QSplitter::handle:horizontal { width: 6px; }
QSplitter::handle:vertical { height: 6px; }
QFrame#Panel { background: #0F1626; border: 1px solid #1C2740; border-radius: 8px; }
QLabel#PanelTitle { font-size: 11px; font-weight: 700; letter-spacing: 1.5px; color: #19D3F5; }
QLabel#PanelSub { color: #8A97AE; }
QLabel#Zoom { color: #8A97AE; font-family: Menlo, Consolas, "Cascadia Mono", monospace; font-size: 11px; }
QLabel#Chip { background: #0E2A3A; color: #7FE9FF; border: 1px solid #1B5A70; border-radius: 9px; padding: 1px 8px; font-size: 11px; }
QFrame#EmptyState { background: #0F1626; border: 1px dashed #26344F; border-radius: 8px; }
QLabel#EmptyTitle { font-size: 15px; font-weight: 600; color: #DCE6F5; }
QLabel#SectionHint { color: #8A97AE; font-size: 11px; }
QTreeWidget { background: #0F1626; border: 1px solid #1C2740; border-radius: 8px; padding: 4px; outline: none; }
QTreeWidget::item { height: 24px; border-radius: 4px; padding-left: 2px; }
QTreeWidget::item:hover { background: #142036; }
QTreeWidget::item:selected { background: #0E2A3A; color: #CFF7FF; }
QGroupBox { background: #0F1626; border: 1px solid #1C2740; border-radius: 8px; margin-top: 18px; padding: 10px 8px 8px 8px; }
QGroupBox::title { subcontrol-origin: margin; left: 10px; top: 2px; color: #8A97AE; font-size: 10px; font-weight: 700; letter-spacing: 1.2px; }
QLineEdit, QSpinBox, QComboBox { background: #080D18; border: 1px solid #22304A; border-radius: 4px; padding: 3px 5px; selection-background-color: #19D3F5; selection-color: #0A0F1C; }
QLineEdit:focus, QSpinBox:focus, QComboBox:focus { border-color: #19D3F5; }
QSpinBox:disabled, QLineEdit:disabled, QComboBox:disabled { color: #56627A; background: #0C1220; border-color: #18223A; }
QSpinBox { font-family: Menlo, Consolas, "Cascadia Mono", monospace; font-size: 11px; }
QComboBox QAbstractItemView { background: #0F1626; border: 1px solid #22304A; selection-background-color: #0E2A3A; }
QPushButton { background: #172238; border: 1px solid #26344F; border-radius: 5px; padding: 5px 10px; }
QPushButton:hover { background: #1D2A44; }
QPushButton:disabled { color: #4E5A72; background: #111A2C; }
QPushButton#Primary { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #19D3F5, stop:1 #7B5CFF); color: #06101F; border-color: #19D3F5; font-weight: 600; }
QPushButton#Primary:hover { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #4FE0FA, stop:1 #957DFF); }
QPushButton#Primary:disabled { background: #0E2A3A; color: #4F7A88; border-color: #16384A; }
QPushButton#Live { background: #FF4D5E; color: white; border-color: #FF4D5E; font-weight: 600; }
QPushButton#Danger { color: #FF8A95; }
QCheckBox::indicator { width: 14px; height: 14px; }
QStatusBar, QWidget#StatusBar { background: #0F1626; border-top: 1px solid #1C2740; }
QLabel#Status { color: #8A97AE; font-family: Menlo, Consolas, "Cascadia Mono", monospace; font-size: 11px; padding: 3px 8px; }
QMenu { background: #111A2C; border: 1px solid #22304A; padding: 4px; }
QMenu::item { padding: 5px 18px; border-radius: 4px; }
QMenu::item:selected { background: #0E2A3A; }
QMenu::item:disabled { color: #4E5A72; }
QToolTip { background: #172238; color: #DCE6F5; border: 1px solid #26344F; }
)");
}

} // namespace om::theme
