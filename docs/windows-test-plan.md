# Windows test plan

The Windows code path (`src/platform/platform-windows.cpp`, plus the shared
watchdog and output code) builds cleanly in CI (MSVC, warnings as errors)
but has **not yet been run on a Windows PC**. Install it from
[Releases](https://github.com/Jallison154/obs-output-mapping/releases) as
described in the README. Work through this list on the target PC.
Everything below is about 45 minutes with two external displays.

Setup: OBS 33.0.1 x64, *Settings ▸ Video ▸ Base (Canvas) Resolution*
`3840x1080`. Windows display mode **Extend**, both externals at native
1920×1080. Create an Image source from `python tools/testpattern.py
pattern.png` (each pixel encodes its own coordinates).

## A. Build and load (no extra displays needed)

| # | Step | Expected |
|---|---|---|
| A1 | Install the release zip to `C:\ProgramData\obs-studio\plugins\output-mapping\` and start OBS | The log shows `[output-mapping] plugin loaded successfully` and `registered obs-websocket vendor`; **Tools ▸ Output Mapping…** exists |
| A2 | Open the editor | The empty state shows; both previews are live; the status bar hints say **Ctrl** / **Alt** (not ⌘/⌥) |
| A3 | Set Windows scaling to 150 %, restart OBS, open the editor | The previews are sharp and correctly sized, and handles line up with the video |

## B. Identity

| # | Step | Expected |
|---|---|---|
| B1 | ＋ Display | Each monitor is listed by its EDID name (e.g. "DELL U2720Q — 1920×1080"); two identical models get distinct "(DISPLAY2)"-style suffixes |
| B2 | Add both externals, then reboot the PC | Both come back **READY** (same device path) |
| B3 | Swap the two HDMI cables between GPU ports | Monitors with an EDID serial are re-identified ("re-identified by serial"); ones without show **CHECK** and fullscreen is blocked until confirmed |
| B4 | Change which display is "main" in Windows settings | No change in identity; outputs keep their displays |

## C. Fullscreen output

| # | Step | Expected |
|---|---|---|
| C1 | Draw a slice over each half, assign one per display, ▶ Start All | Each display shows only its half: no taskbar, no title bar, no cursor, no Alt-Tab entry, and the OBS window keeps focus |
| C2 | Inspect the seam (loupe, camera, or HDMI capture) | The left display's last column is Program x=1919 and the right display's first is x=1920; no blur |
| C3 | Snapshot… each display and compare with the pattern | Identical to the crop |
| C4 | With 150 % scaling on an output display | Still exactly 1:1 (the output uses physical pixels) |
| C5 | Add a Display Capture of an output monitor to a scene | With *Options ▸ Hide outputs from screen capture* on (default), the capture shows the desktop behind it, never the output (no feedback tunnel) |
| C6 | Click on an output display, press Esc | Nothing happens (show-safe). Stop it from the editor instead |
| C7 | Start fullscreen on the monitor OBS is on (confirm the prompt), click it, press Esc | It stops (escape hatch) |
| C8 | Switch scenes with a Fade | Both outputs follow, including mid-fade |
| C10 | Display panel ▸ Identify ▸ **Test Pattern** on each live display (confirm the prompt) | The red outermost line and the white second line are visible on all four edges. If not, the TV/projector is overscanning: turn on "Just Scan" / "1:1" / "Screen Fit" in its menu. The centre cross sits at the screen centre. Toggle off: video returns instantly |
| C11 | **Slice Outlines** while live | Each slice's outline and name lines up with the content on the real screen |
| C12 | Click **■ Stop All** with outputs live | A confirmation appears; Cancel keeps everything running |
| C9 | Stream and record for 30 minutes with outputs live | No dropped frames attributable to outputs; render time in *Stats* stays reasonable |

## D. Failure handling

| # | Step | Expected |
|---|---|---|
| D1 | Unplug the right display while it's live | Its output disappears immediately; nothing flashes on the remaining screens; status turns **OFFLINE** |
| D2 | Plug it back into the same port | The output resumes on it within about a second |
| D3 | Turn the TV off with its remote (many TVs drop HDMI hot-plug) | Same as D1/D2 when it turns back on |
| D4 | Change an output display's resolution while live | The output is re-placed at the new size (watchdog); the display shows the new pixels |
| D5 | Quit OBS with both outputs live, start OBS | Both are restored on the correct displays about 1.5 s after load |
| D6 | Quit OBS, unplug one display, start OBS | That display is **OFFLINE** and nothing opens elsewhere; the other is restored |
| D7 | Corrupt `%APPDATA%\obs-studio\plugin_config\output-mapping\mappings.json` (type garbage into it), start OBS | The log says it was moved to `mappings.json.unreadable-…`; the editor starts empty; a `.bak` with the last good mapping exists |
| D8 | Leave the PC idle past the display-sleep timeout with outputs live | Displays stay awake |

## E. Editor UX

| # | Step | Expected |
|---|---|---|
| E1 | Mouse wheel over a preview | Zooms at the cursor; Space-drag or middle-drag pans |
| E2 | Draw, move and resize slices with snapping; hold Alt | Snaps to edges, centre and other slices; Alt bypasses |
| E3 | Delete a slice with Del, then Ctrl+Z / Ctrl+Y | Comes back exactly / is removed again |
| E4 | Drag a slice in the sidebar onto another display | It moves to that display, on top |
| E5 | Type W/H values with Keep Aspect on | The other value follows the aspect |
| E6 | Ctrl+C / Ctrl+V a slice; select another display and Ctrl+V | Same display: offset copy. Other display: same crop placed there |
| E7 | Ctrl+Shift+V onto another slice | It takes the copied slice's exact output position |
| E8 | Tab, ] / [, H, F2, Esc in a preview | Next slice, layer order, show/hide, rename, deselect |
| E9 | Lock a slice, try to drag / nudge / delete it | Nothing moves; the status bar says it's locked |

Report anything that fails with the OBS log
(*Help ▸ Log Files ▸ Upload Current Log File*, or the file in
`%APPDATA%\obs-studio\logs`). All plugin lines start with `[output-mapping]`.
