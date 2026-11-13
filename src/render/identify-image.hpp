/*
Output Mapping for OBS Studio
Copyright (C) 2026 Okami Designs

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// CPU-drawn identify overlays for an output (UI thread, QPainter):
//  - test pattern: full-frame calibration image (edges, grid, rulers,
//    corners, centre, display name/resolution) plus slice outlines
//  - outlines: transparent overlay with slice outlines and labels

#pragma once

#include "model/mapping.hpp"

#include <QImage>

namespace om {

/* withSlices=false: clean pattern for the editor preview (the editor draws
 * its own interactive outlines on top). */
QImage makeTestPattern(const DisplayMapping &d, bool withSlices = true);
QImage makeOutlineOverlay(const DisplayMapping &d);

} // namespace om
