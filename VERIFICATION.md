# Verification record

## Environment actually tested

| | |
|---|---|
| Machine | Apple Silicon (M3) laptop, **one display** (built-in Retina, 3420×2224 backing) |
| macOS | 27.0.1 (build 26A434) |
| OBS Studio | **33.0.1** official macOS Apple Silicon build (obs-websocket 5.7.6, Qt 6.11.1), OpenGL renderer (default) |
| Plugin build | Xcode 27.1, CMake 4.5.0, against OBS 33.0.1 sources plus obs-deps / Qt 2026-10-23; universal binary, ad-hoc signed |
| Date | 2026-11-12 |

No other OBS versions were tested. The plugin uses only APIs that have been
stable since OBS 30 (`obs_get_main_texture`, `obs_display_*`,
`obs_add_main_rendered_callback`, the frontend Tools-menu and hotkey APIs),
but it is built against the 33.0.1 headers and Qt 6.11, so treat other
versions as untested.

## Automated

- `output-mapping-core-tests`: geometry (draw, move, resize, aspect lock,
  snapping, numeric edits, fit, 1:1, clipping) and display identity
  matching (exact UUID, serial re-identification, twin units, missing
  serials, duplicate UUIDs). **All pass.**
- `tools/acceptance_test.py` against a real OBS 33.0.1: **49/49 checks pass** (full log: [docs/results/acceptance-run.txt](docs/results/acceptance-run.txt))

The acceptance test runs the six steps of the requested acceptance test.
Since only one physical display was available, the two 1920×1080 outputs
are **virtual displays opened as windows**. Pixels are verified with
**Snapshot**, which renders through the same `gfx::renderDisplay()` call
the fullscreen outputs use, on the GPU, inside a real OBS frame. The
Program source is a 3840×1080 pattern in which every pixel encodes its own
coordinates (a 1 px shift changes the value by 67).

| Step | What was checked | Result |
|---|---|---|
| 1 | Program canvas set to 3840×1080; two 1920×1080 outputs | ✅ |
| 2 | Slices drawn over each half **by dragging in Input Selection** (synthetic mouse events through the real editor handlers); edges snap to 0/1920/3840/1080; each lands on its own display at 1:1 | ✅ |
| 2 | HDMI-1 = Program x 0–1919 and HDMI-2 = Program x 1920–3839, **every pixel, max difference 0**; seam columns are exactly x=1919 and x=1920 | ✅ |
| 3 | Dragging Slice 1 in Input Selection moves its crop by (+100,+50); destination unchanged; output pixel-exact, with rows beyond the canvas black | ✅ |
| 4 | Dragging Slice 2 in Output Placement moves it to (200,100); crop unchanged; output pixel-exact with a black margin. Arrow-key nudge (+3, ⇧+10) | ✅ |
| 5 | Fade to Scene B: **mid-transition** both outputs show a blend of A and B; afterwards they are pixel-exact Scene B. Stopping HDMI-1 leaves HDMI-2 running and following Program | ✅ |
| 6 | OBS quit and relaunched: both slices' source and destination persist exactly; display list persists | ✅ |
| 6 | A saved display that isn't connected shows **Disconnected**. A same-model display without a proven identity shows **Ambiguous**. Fullscreen is refused on both before and after the restart, and their pending restore is held back instead of going to another screen | ✅ |
| extra | **Real borderless fullscreen** output opened on the built-in display and kept in place by the placement watchdog, restored automatically after an OBS restart (exact UUID match), and its restore flag cleared when the user stops it | ✅ |
| extra | Delete a slice with the Delete key, Undo restores it exactly, Redo removes it again | ✅ |

The editor screenshots in `docs/images/` were captured by the plugin's
`EditorCapture`, which composites each live preview, rendered through its
own GPU draw path, into a Qt grab of the window. That avoids needing
macOS Screen Recording permission.

## Bugs found by this verification and fixed

1. **Restore recursion → crash on launch.** Resuming a saved fullscreen
   output re-scanned the screens, which resumed again, and so on until the
   stack overflowed. Fixed: rescanning never resumes, plus a re-entrancy
   guard.
2. **Quitting OBS cleared the restore flag.** Qt closes every window at
   quit, and the fullscreen output treated that like a user stop. Fixed:
   borderless outputs treat external closes as an app quit and keep the
   flag.
3. **Rects lost over obs-websocket.** `obs_data` can't carry arrays of plain
   numbers. Rects are now `{x,y,w,h}` objects; the legacy array form still
   loads.
4. The editor opened partly off-screen, and the sidebar inspector was
   clipped. Fixed.
5. (Found in review, Windows.) Falling back to GDI names (`\\.\DISPLAYn`)
   as an identity could have matched a different monitor after Windows
   renumbered them. Those displays now never get an exact identity; they
   show as ambiguous until you confirm them for the session.

## Windows: not compiled or run yet

The Windows display layer (`src/platform/platform-windows.cpp`) and the
Windows paths in the shared code were written on macOS and **have not been
compiled with MSVC or run on Windows**. They follow the same DisplayConfig
calls OBS's own Windows monitor capture uses. The template's GitHub Actions
workflow builds them on `windows-2022` with warnings as errors. The full
on-device checklist is [docs/windows-test-plan.md](docs/windows-test-plan.md).

## macOS: not verified here, needs real hardware

See [docs/hardware-test-checklist.md](docs/hardware-test-checklist.md). In short:

- Two **simultaneous fullscreen** outputs on **two external HDMI displays**,
  and the 1:1 seam as it appears on the actual panels and signal. Only
  windowed outputs and one built-in fullscreen output were tested.
- Behaviour with *Displays have separate Spaces* turned off.
- **Hot-unplug and replug** while live: the code closes the output on
  `QGuiApplication::screenRemoved` before macOS can move the window, and
  resumes on exact reconnection. Neither path could be exercised without a
  second display.
- EDID serial re-identification when a display moves to another port. This
  is covered by unit tests only.
- Scaled ("looks like") modes on an external display, the Metal renderer,
  Intel Macs (the x86_64 slice is built but not run), Windows and Linux.

## Environment notes

- OBS 33.0.1 sometimes takes 30–60 s to quit, because its main window
  waits for its own *What's New* network thread. This is unrelated to the
  plugin. The harness waits up to 90 s before force-killing.
- Building inside an iCloud-synced folder breaks `codesign`; see the
  README.
