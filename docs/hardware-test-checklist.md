# macOS hardware test checklist (needs two real external displays)

For Windows, see [windows-test-plan.md](windows-test-plan.md).

These are the behaviours that could not be verified on the single-display
development machine. Each one takes a few minutes with real hardware.

Setup: OBS canvas 3840×1080. Two external displays (HDMI/DisplayPort),
each at its native 1920×1080 in *System Settings ▸ Displays* (not a
"scaled" mode). Use the test pattern from `python3 tools/testpattern.py
pattern.png` as an Image source.

| # | Check | Expected |
|---|---|---|
| H1 | ＋ Display lists both externals with the correct name and `1920×1080` | Both appear with a green *Connected* dot |
| H2 | Start Fullscreen on both | Each display shows only its half; no menu bar, Dock, title bar or cursor; no Space-switch animation |
| H3 | Inspect the seam with a loupe or camera, or capture the HDMI signal | Display 1's right edge is Program x=1919 and display 2's left edge is x=1920; no blur, no doubled or missing column |
| H4 | Same as H2 with *Displays have separate Spaces* **off** (System Settings ▸ Desktop & Dock), log out and back in | Both fullscreen outputs still work at once |
| H5 | Stop one output with the editor button, then with Esc on the output | The other output keeps running |
| H6 | Unplug display 2 while it is live | Its output closes immediately and does **not** appear on display 1 or the laptop screen; status turns *Disconnected* |
| H7 | Plug display 2 back into the **same** port | Status is *Connected* again and the output resumes automatically |
| H8 | Plug display 2 into a **different** port | Resumes (re-identified by serial) if the display reports an EDID serial; otherwise it shows *Ambiguous* and stays off until you reassign it |
| H9 | Swap in two identical monitors without serials | They show *Ambiguous*; nothing opens until each is reassigned |
| H10 | Quit OBS with both outputs live and relaunch | Both outputs come back on the correct displays about 1.5 s after OBS loads |
| H11 | Quit OBS, unplug display 2, relaunch | Display 2 shows *Disconnected*, nothing opens on another screen, and display 1 still restores |
| H12 | Set one display to a scaled ("looks like") mode | The editor warns that output is not 1:1 |
| H13 | Run for 30+ minutes while streaming and recording | Mapped outputs unaffected; check that OBS's *Stats* render time stays reasonable |
| H14 | Settings ▸ Advanced ▸ Renderer = **Metal** (OBS 33 option), restart | Outputs and previews render identically (verification used OpenGL, the default) |
