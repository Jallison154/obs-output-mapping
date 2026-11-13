#!/usr/bin/env python3
"""End-to-end acceptance test for the Output Mapping plugin.

Drives a real OBS Studio instance through obs-websocket and the plugin's
"output-mapping" vendor requests, following the acceptance test in the
README:

  1. live 3840x1080 Program canvas, two 1920x1080 outputs
  2. draw a slice over each half with the (synthetic) mouse, assign one per
     display, verify 1:1 pixel alignment against a coordinate-encoded pattern
  3. drag a source rect in Input Selection -> crop changes, destination fixed
  4. drag the other slice in Output Placement -> placement changes, crop fixed
  5. scene switch with a Fade transition: outputs follow Program (checked
     mid-transition and after); stop one output, the other stays active
  6. restart OBS: mapping persists; a missing display shows as disconnected
     and an ambiguous one is refused; nothing goes fullscreen on them

Physical HDMI displays are not required: the two outputs are virtual
1920x1080 displays opened as windows. Snapshots are rendered by the exact
same GPU path as the fullscreen outputs. If --fullscreen-builtin is given,
a real borderless fullscreen output is also opened on the built-in display
for a few seconds and restored across a restart.

Usage:
  python3 tools/acceptance_test.py --obs-app ~/Library/Caches/obs-output-mapping/OBS.app --work /tmp/omtest
"""

import argparse
import json
import os
import secrets
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import obsws  # noqa: E402
from testpattern import pattern_pixel, write_png  # noqa: E402

# Tests always run OBS in an ISOLATED home (its own settings, plugins and
# websocket port), so they never touch a user's real OBS or its settings,
# even if that OBS is running at the same time.
TEST_HOME = None
OBS_CONFIG = None
DEFAULT_PORT = 4466
DEFAULT_PLUGIN = os.path.expanduser(
    "~/Library/Caches/obs-output-mapping/build_macos/RelWithDebInfo/output-mapping.plugin")


def set_test_home(home):
    """Use `home` as the isolated user home for the test OBS."""
    global TEST_HOME, OBS_CONFIG
    TEST_HOME = os.path.abspath(home)
    OBS_CONFIG = os.path.join(TEST_HOME, "Library", "Application Support", "obs-studio")
    os.makedirs(OBS_CONFIG, exist_ok=True)


def install_plugin(bundle=DEFAULT_PLUGIN):
    """Install the plugin build into the isolated OBS (never the real one)."""
    import shutil
    dst = os.path.join(OBS_CONFIG, "plugins", "output-mapping.plugin")
    if os.path.exists(dst):
        shutil.rmtree(dst)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    subprocess.run(["ditto", bundle, dst], check=True)


def reset_plugin_state():
    """Remove the isolated OBS's saved mappings (test data only)."""
    d = os.path.join(OBS_CONFIG, "plugin_config", "output-mapping")
    for name in ("mappings.json", "mappings.json.bak"):
        p = os.path.join(d, name)
        if os.path.exists(p):
            os.remove(p)
TAG_A, TAG_B = 40, 200
RESULTS = []


def log(msg):
    print(msg, flush=True)


def check(name, cond, detail=""):
    RESULTS.append((name, bool(cond), detail))
    log(f"  [{'PASS' if cond else 'FAIL'}] {name}" + (f" — {detail}" if detail else ""))
    return cond


# ---------------------------------------------------------------- OBS process
def seed_config(port, password):
    os.makedirs(OBS_CONFIG, exist_ok=True)
    gi = os.path.join(OBS_CONFIG, "global.ini")
    if not os.path.exists(gi):
        with open(gi, "w") as f:
            # 33.0.1 = (33 << 24) | 1 : suppresses first-run wizard / what's new
            f.write("[General]\nLastVersion=553648129\nPre31Migrated=true\n")
    ui = os.path.join(OBS_CONFIG, "user.ini")
    if not os.path.exists(ui):
        with open(ui, "w") as f:
            f.write("[General]\nFirstRun=true\n\n[Basic]\nProfile=Output Mapping Test\n"
                    "ProfileDir=Output_Mapping_Test\nSceneCollection=Output Mapping Test\n"
                    "SceneCollectionFile=Output_Mapping_Test\n")
    ws_dir = os.path.join(OBS_CONFIG, "plugin_config", "obs-websocket")
    os.makedirs(ws_dir, exist_ok=True)
    cfg_path = os.path.join(ws_dir, "config.json")
    cfg = {}
    if os.path.exists(cfg_path):
        with open(cfg_path) as f:
            cfg = json.load(f)
    # Auth stays ON: a random per-run password, never an open server.
    cfg.update({"server_enabled": True, "server_port": port, "auth_required": True,
                "server_password": password, "alerts_enabled": False, "first_load": False})
    with open(cfg_path, "w") as f:
        json.dump(cfg, f, indent=2)


def clear_unclean_marker():
    """OBS 33 removed --disable-shutdown-check: after a crash or forced quit
    it blocks on a "launch in Safe Mode?" prompt. Remove stale session
    markers (only called while no OBS is running)."""
    d = os.path.join(OBS_CONFIG, ".sentinel")
    if os.path.isdir(d):
        for name in os.listdir(d):
            if name.startswith("run_"):
                os.remove(os.path.join(d, name))


def launch_obs(app, logfile):
    assert TEST_HOME, "call set_test_home() first"
    clear_unclean_marker()
    # CFFIXED_USER_HOME makes OBS use the isolated home for all its settings;
    # --multi lets it run next to a user's own OBS instance.
    env = dict(os.environ, OUTPUT_MAPPING_AUTOMATION="1", OUTPUT_MAPPING_OPEN_EDITOR="1",
               CFFIXED_USER_HOME=TEST_HOME, HOME=TEST_HOME)
    exe = os.path.join(app, "Contents", "MacOS", "OBS")
    out = open(logfile, "a")
    return subprocess.Popen([exe, "--multi", "--disable-updater",
                             "--disable-missing-files-check"],
                            cwd=os.path.join(app, "Contents", "MacOS"), env=env, stdout=out, stderr=out)


def quit_obs(proc):
    """Quit ONLY the OBS process this harness started (never by app id, which
    would also target a user's own OBS). SIGTERM asks OBS to shut down
    cleanly; it can take a while because OBS waits for its What's New
    network thread, so allow 90 s before force-killing."""
    import signal
    if proc.poll() is not None:
        return True
    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=90)
        log("  OBS exited cleanly")
        return True
    except subprocess.TimeoutExpired:
        log("  OBS did not exit within 90 s (stuck in its own shutdown); force-killing")
        proc.kill()
        proc.wait(timeout=10)
        return False


# ---------------------------------------------------------------- image checks
def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    parts, pos = [], 0
    while len(parts) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            pos = data.index(b"\n", pos) + 1
            continue
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        parts.append(data[pos:end])
        pos = end
    assert parts[0] == b"P6"
    w, h = int(parts[1]), int(parts[2])
    return w, h, data[pos + 1:]


def compare(path, expected_fn, w=1920, h=1080, tol=2, step=1):
    """expected_fn(x, y) -> (r,g,b). Returns (mismatches, maxdiff, checked)."""
    iw, ih, px = read_ppm(path)
    if (iw, ih) != (w, h):
        return (-1, -1, 0)
    bad = maxd = n = 0
    first = None
    for y in range(0, h, step):
        row = y * w * 3
        for x in range(0, w, step):
            i = row + x * 3
            e = expected_fn(x, y)
            d = max(abs(px[i] - e[0]), abs(px[i + 1] - e[1]), abs(px[i + 2] - e[2]))
            n += 1
            if d > maxd:
                maxd = d
            if d > tol:
                bad += 1
                if first is None:
                    first = (x, y, tuple(px[i:i + 3]), e)
    return bad, maxd, n, first


def pixel(path, x, y, w=1920):
    _, _, px = read_ppm(path)
    i = (y * w + x) * 3
    return tuple(px[i:i + 3])


def expect_crop(src_x, src_y, dst_x, dst_y, tag, canvas_w=3840, canvas_h=1080, w=1920, h=1080):
    """Display pixel (x,y) shows Program pixel (x-dst_x+src_x, y-dst_y+src_y) at 1:1; black elsewhere."""
    def fn(x, y):
        if x < dst_x or y < dst_y or x >= dst_x + w or y >= dst_y + h:
            return (0, 0, 0)
        px, py = x - dst_x + src_x, y - dst_y + src_y
        if px < 0 or py < 0 or px >= canvas_w or py >= canvas_h:
            return (0, 0, 0)
        return pattern_pixel(px, py, tag)
    return fn


# ---------------------------------------------------------------- helpers
def R(x, y, w, h):
    return {"x": x, "y": y, "w": w, "h": h}


def rl(r):
    return [r["x"], r["y"], r["w"], r["h"]]


def between(v, a, b, margin=5):
    lo, hi = min(a, b), max(a, b)
    return lo + margin < v < hi - margin


def state(ws):
    return ws.vendor("GetState")["state"]


def disp(st, name):
    return next(d for d in st["displays"] if d["name"] == name)


def slice_by_name(st, name):
    for d in st["displays"]:
        for s in d["slices"]:
            if s["name"] == name:
                return d, s
    raise KeyError(name)


def snapshot(ws, name, path):
    r = ws.vendor("Snapshot", {"displayName": name, "path": path})
    if not r.get("ok"):
        raise RuntimeError(f"snapshot {name}: {r}")
    return path


def setup_obs(ws, work):
    log("Setting up OBS: 3840x1080 canvas, two test-pattern scenes, Fade transition")
    pa, pb = os.path.join(work, "pattern_a.png"), os.path.join(work, "pattern_b.png")
    if not os.path.exists(pa):
        write_png(pa, 3840, 1080, lambda x, y: pattern_pixel(x, y, TAG_A))
        write_png(pb, 3840, 1080, lambda x, y: pattern_pixel(x, y, TAG_B))
    cols = ws.call("GetSceneCollectionList")
    if "Output Mapping Test" not in cols["sceneCollections"]:
        ws.call("CreateSceneCollection", {"sceneCollectionName": "Output Mapping Test"})
        time.sleep(2)
    elif cols["currentSceneCollectionName"] != "Output Mapping Test":
        ws.call("SetCurrentSceneCollection", {"sceneCollectionName": "Output Mapping Test"})
        time.sleep(2)
    ws.call("SetVideoSettings", {"baseWidth": 3840, "baseHeight": 1080, "outputWidth": 1920,
                                 "outputHeight": 540, "fpsNumerator": 30, "fpsDenominator": 1})
    scenes = [s["sceneName"] for s in ws.call("GetSceneList")["scenes"]]
    for scene, png in (("Scene A", pa), ("Scene B", pb)):
        if scene not in scenes:
            ws.call("CreateScene", {"sceneName": scene})
            ws.call("CreateInput", {"sceneName": scene, "inputName": f"Pattern {scene[-1]}",
                                    "inputKind": "image_source", "inputSettings": {"file": png}})
    ws.call("SetCurrentProgramScene", {"sceneName": "Scene A"})
    ws.call("SetCurrentSceneTransition", {"transitionName": "Fade"})
    ws.call("SetCurrentSceneTransitionDuration", {"transitionDuration": 1500})
    time.sleep(1.5)
    vs = ws.call("GetVideoSettings")
    check("Program canvas is 3840x1080", vs["baseWidth"] == 3840 and vs["baseHeight"] == 1080,
          f"{vs['baseWidth']}x{vs['baseHeight']}")


# ---------------------------------------------------------------- phases
def phase_mapping(ws, work, fullscreen_builtin):
    log("\n== 1/2  Two 1920x1080 outputs, slices drawn with the mouse ==")
    doc = {"format": "obs-output-mapping", "version": 1, "restoreOutputs": True, "displays": [
        {"id": "hdmi-1", "name": "HDMI-1", "kind": "virtual", "width": 1920, "height": 1080, "slices": []},
        {"id": "hdmi-2", "name": "HDMI-2", "kind": "virtual", "width": 1920, "height": 1080, "slices": []},
    ]}
    ws.vendor("SetState", {"state": doc})
    ws.vendor("OpenEditor", {"width": 1500, "height": 860})
    time.sleep(1.5)

    ws.vendor("Select", {"displayName": "HDMI-1"})
    ws.vendor("EditorDrag", {"view": "input", "fromX": 4, "fromY": 6, "toX": 1915, "toY": 1076})
    ws.vendor("Select", {"displayName": "HDMI-2"})
    ws.vendor("EditorDrag", {"view": "input", "fromX": 3836, "fromY": 1075, "toX": 1924, "toY": 3})
    st = state(ws)
    d1, s1 = slice_by_name(st, "Slice 1")
    d2, s2 = slice_by_name(st, "Slice 2")
    check("Slice 1 drawn over left half (snapped)", rl(s1["src"]) == [0, 0, 1920, 1080], str(rl(s1["src"])))
    check("Slice 2 drawn over right half (snapped)", rl(s2["src"]) == [1920, 0, 1920, 1080], str(rl(s2["src"])))
    check("Slice 1 assigned to HDMI-1 at 1:1", d1["name"] == "HDMI-1" and rl(s1["dst"]) == [0, 0, 1920, 1080],
          f"{d1['name']} {s1['dst']}")
    check("Slice 2 assigned to HDMI-2 at 1:1", d2["name"] == "HDMI-2" and rl(s2["dst"]) == [0, 0, 1920, 1080],
          f"{d2['name']} {s2['dst']}")

    for n in ("HDMI-1", "HDMI-2"):
        ws.vendor("OpenWindow", {"displayName": n})
    time.sleep(1.0)

    time.sleep(2.5)  # health is sampled once a second
    st = state(ws)
    fps = {d["name"]: d.get("outputFps") for d in st["displays"]}
    check("Output windows really draw frames on screen (not just snapshots)",
          all((fps.get(n) or 0) >= 10 for n in ("HDMI-1", "HDMI-2")), str(fps))
    a = snapshot(ws, "HDMI-1", os.path.join(work, "step2_hdmi1.ppm"))
    b = snapshot(ws, "HDMI-2", os.path.join(work, "step2_hdmi2.ppm"))
    r = compare(a, expect_crop(0, 0, 0, 0, TAG_A))
    check("HDMI-1 == Program[0..1919] pixel-for-pixel", r[0] == 0, f"mismatches={r[0]} maxdiff={r[1]} of {r[2]} px")
    r = compare(b, expect_crop(1920, 0, 0, 0, TAG_A))
    check("HDMI-2 == Program[1920..3839] pixel-for-pixel", r[0] == 0, f"mismatches={r[0]} maxdiff={r[1]} of {r[2]} px")
    # Seam: last column of HDMI-1 and first of HDMI-2 are Program x=1919 and x=1920.
    check("Seam columns are Program x=1919 | x=1920",
          pixel(a, 1919, 500) == pattern_pixel(1919, 500, TAG_A) and pixel(b, 0, 500) == pattern_pixel(1920, 500, TAG_A),
          f"{pixel(a, 1919, 500)} | {pixel(b, 0, 500)}")

    log("\n== 3  Drag source rect in Input Selection (destination fixed) ==")
    ws.vendor("Select", {"sliceName": "Slice 1"})
    ws.vendor("EditorDrag", {"view": "input", "fromX": 960, "fromY": 540, "toX": 1060, "toY": 590, "alt": True})
    st = state(ws)
    _, s1b = slice_by_name(st, "Slice 1")
    check("Slice 1 source moved by (+100,+50)", rl(s1b["src"]) == [100, 50, 1920, 1080], str(s1b["src"]))
    check("Slice 1 destination unchanged", s1b["dst"] == s1["dst"], str(s1b["dst"]))
    a = snapshot(ws, "HDMI-1", os.path.join(work, "step3_hdmi1.ppm"))
    r = compare(a, expect_crop(100, 50, 0, 0, TAG_A))
    check("HDMI-1 now shows Program from (100,50); rows past canvas are black", r[0] == 0,
          f"mismatches={r[0]} maxdiff={r[1]}")

    log("\n== 4  Drag destination rect in Output Placement (source fixed) ==")
    ws.vendor("Select", {"sliceName": "Slice 2"})
    ws.vendor("EditorDrag", {"view": "output", "fromX": 960, "fromY": 540, "toX": 1160, "toY": 640, "alt": True})
    st = state(ws)
    _, s2b = slice_by_name(st, "Slice 2")
    check("Slice 2 destination moved by (+200,+100)", rl(s2b["dst"]) == [200, 100, 1920, 1080], str(s2b["dst"]))
    check("Slice 2 source unchanged", s2b["src"] == s2["src"], str(s2b["src"]))
    b = snapshot(ws, "HDMI-2", os.path.join(work, "step4_hdmi2.ppm"))
    r = compare(b, expect_crop(1920, 0, 200, 100, TAG_A))
    check("HDMI-2 shows the same crop shifted to (200,100), black margin", r[0] == 0,
          f"mismatches={r[0]} maxdiff={r[1]}")

    # Arrow-key nudge on the output view: Right x3 (+3px), Shift+Down (+10px).
    for _ in range(3):
        ws.vendor("EditorKey", {"view": "output", "key": 0x01000014})  # Qt::Key_Right
    ws.vendor("EditorKey", {"view": "output", "key": 0x01000015, "shift": True})  # Qt::Key_Down
    _, s2c = slice_by_name(state(ws), "Slice 2")
    check("Arrow-key nudge moves destination (+3,+10)", rl(s2c["dst"]) == [203, 110, 1920, 1080], str(s2c["dst"]))
    for _ in range(3):
        ws.vendor("EditorKey", {"view": "output", "key": 0x01000012})  # Left
    ws.vendor("EditorKey", {"view": "output", "key": 0x01000013, "shift": True})  # Up

    # Undo / redo: delete Slice 2 with the Delete key, then undo.
    time.sleep(0.8)  # let the nudges' undo group close
    ws.vendor("Select", {"sliceName": "Slice 2"})
    ws.vendor("EditorKey", {"view": "output", "key": 0x01000007})  # Qt::Key_Delete
    gone = all(s["name"] != "Slice 2" for d in state(ws)["displays"] for s in d["slices"])
    check("Delete key removes the selected slice", gone)
    ws.vendor("Undo")
    _, s2u = slice_by_name(state(ws), "Slice 2")
    check("Undo restores the deleted slice exactly", s2u["src"] == s2b["src"] and s2u["dst"] == s2b["dst"],
          f"{rl(s2u['src'])} {rl(s2u['dst'])}")
    ws.vendor("Redo")
    gone = all(s["name"] != "Slice 2" for d in state(ws)["displays"] for s in d["slices"])
    ws.vendor("Undo")
    check("Redo re-applies, undo brings it back", gone and any(
        s["name"] == "Slice 2" for d in state(ws)["displays"] for s in d["slices"]))

    # Per-output video delay: 500 ms on HDMI-2 only, verified with a hard cut.
    ws.call("SetCurrentSceneTransition", {"transitionName": "Cut"})
    ws.call("SetCurrentProgramScene", {"sceneName": "Scene A"})
    r = ws.vendor("SetDelay", {"displayName": "HDMI-2", "delayMs": 500})
    check("Set 500 ms delay on HDMI-2", r.get("ok") and disp(r["state"], "HDMI-2")["delayMs"] == 500)

    def no_black_while(label, seconds):
        bad, n, t_end = 0, 0, time.time() + seconds
        while time.time() < t_end:
            p = snapshot(ws, "HDMI-2", os.path.join(work, "ramp.ppm"))
            n += 1
            if pixel(p, 400, 400) == (0, 0, 0) or pixel(p, 1500, 900) == (0, 0, 0):
                bad += 1
        check(label, bad == 0 and n >= 5, f"{bad} black of {n} frames sampled")

    no_black_while("Turning delay on never shows black (smooth ramp)", 1.2)
    time.sleep(0.5)  # ramp to 500 ms completes (~1 s at 30 fps)
    ws.call("SetCurrentProgramScene", {"sceneName": "Scene B"})
    t0 = time.time()
    time.sleep(0.15)
    a = snapshot(ws, "HDMI-1", os.path.join(work, "delay_hdmi1_early.ppm"))
    b = snapshot(ws, "HDMI-2", os.path.join(work, "delay_hdmi2_early.ppm"))
    early = time.time() - t0
    tag = lambda path, x, y, sx, sy: pixel(path, x, y)[2]
    check("Undelayed HDMI-1 switched to Scene B immediately",
          pixel(a, 10, 10) == pattern_pixel(110, 60, TAG_B), f"{pixel(a, 10, 10)}")
    check("Delayed HDMI-2 still shows Scene A %.0f ms after the cut" % (early * 1000),
          pixel(b, 400, 400) == pattern_pixel(1920 + 200, 300, TAG_A), f"{pixel(b, 400, 400)}")
    time.sleep(0.8)
    b = snapshot(ws, "HDMI-2", os.path.join(work, "delay_hdmi2_late.ppm"))
    r = compare(b, expect_crop(1920, 0, 200, 100, TAG_B), step=4)
    check("Delayed HDMI-2 shows Scene B after the delay, pixel-exact", r[0] == 0, f"mismatches={r[0]}")
    ws.vendor("SetDelay", {"displayName": "HDMI-2", "delayMs": 300})
    no_black_while("Changing delay 500 -> 300 ms live never shows black", 0.8)
    ws.vendor("SetDelay", {"displayName": "HDMI-2", "delayMs": 0})
    no_black_while("Turning delay off never shows black", 0.8)
    b = snapshot(ws, "HDMI-2", os.path.join(work, "delay_off.ppm"))
    r = compare(b, expect_crop(1920, 0, 200, 100, TAG_B), step=4)
    check("After turning delay off, HDMI-2 is live and pixel-exact", r[0] == 0, f"mismatches={r[0]}")
    ws.call("SetCurrentProgramScene", {"sceneName": "Scene A"})  # still a Cut: instant
    time.sleep(0.3)
    ws.call("SetCurrentSceneTransition", {"transitionName": "Fade"})
    ws.call("SetCurrentSceneTransitionDuration", {"transitionDuration": 1500})
    time.sleep(0.5)

    # Copy / paste (same code paths as the keyboard shortcuts).
    def names(st):
        return [s["name"] for d in st["displays"] for s in d["slices"]]
    ws.vendor("Select", {"sliceName": "Slice 2"})
    ws.vendor("EditorAction", {"name": "copy"})
    r = ws.vendor("EditorAction", {"name": "paste"})
    dc, cp = slice_by_name(r["state"], "Slice 2 copy")
    _, orig = slice_by_name(r["state"], "Slice 2")
    check("Paste onto the same display: offset copy, same crop",
          dc["name"] == "HDMI-2" and cp["src"] == orig["src"]
          and rl(cp["dst"])[:2] == [rl(orig["dst"])[0] + 20, rl(orig["dst"])[1] + 20],
          f"{dc['name']} {rl(cp['dst'])}")
    r = ws.vendor("EditorAction", {"name": "pastePosition"})
    _, cp2 = slice_by_name(r["state"], "Slice 2 copy")
    check("Paste Position Only copies the output rect exactly", cp2["dst"] == orig["dst"], str(rl(cp2["dst"])))
    ws.vendor("Undo"); ws.vendor("Undo")
    check("Undo removes the pasted slice", "Slice 2 copy" not in names(state(ws)))
    ws.vendor("Select", {"displayName": "HDMI-1"})
    r = ws.vendor("EditorAction", {"name": "paste"})
    dc, cp = slice_by_name(r["state"], "Slice 2 copy")
    check("Paste onto another display keeps the crop", dc["name"] == "HDMI-1" and cp["src"] == orig["src"],
          f"{dc['name']} {rl(cp['src'])}")
    ws.vendor("Undo")
    ws.vendor("Select", {"sliceName": "Slice 1"})
    r = ws.vendor("EditorKey", {"view": "output", "key": 0x01000001})  # Tab
    check("Tab moves to the next slice", r["state"]["selectedSlice"] != slice_by_name(r["state"], "Slice 1")[1]["id"])

    # Identify: test pattern and outlines on the real output, instantly.
    r = ws.vendor("SetIdentify", {"displayName": "HDMI-2", "mode": "pattern"})
    check("Test pattern on HDMI-2", disp(r["state"], "HDMI-2")["identify"] == "pattern")
    time.sleep(0.3)
    p = snapshot(ws, "HDMI-2", os.path.join(work, "identify_pattern.ppm"))
    near = lambda a, b: max(abs(x - y) for x, y in zip(a, b)) <= 3
    edges = [pixel(p, 0, 0), pixel(p, 1919, 0), pixel(p, 0, 1079), pixel(p, 1919, 1079), pixel(p, 960, 0)]
    check("Test pattern: outermost pixels red on every edge", all(near(e, (255, 40, 40)) for e in edges), str(edges))
    check("Test pattern: second pixel row white (edge-crop check)",
          near(pixel(p, 960, 1), (255, 255, 255)) and near(pixel(p, 1, 540), (255, 255, 255)),
          f"{pixel(p, 960, 1)} {pixel(p, 1, 540)}")
    check("Test pattern replaces the video", pixel(p, 1500, 900) != pattern_pixel(1920 + 1300, 800, TAG_A),
          str(pixel(p, 1500, 900)))
    ws.vendor("SetIdentify", {"displayName": "HDMI-2", "mode": "outlines"})
    time.sleep(0.3)
    p = snapshot(ws, "HDMI-2", os.path.join(work, "identify_outlines.ppm"))
    check("Slice outlines: display edge marked", near(pixel(p, 0, 540), (255, 196, 66)), str(pixel(p, 0, 540)))
    check("Slice outlines: live video still visible inside the slice",
          max(abs(a - b) for a, b in zip(pixel(p, 1500, 600), pattern_pixel(1920 + 1300, 500, TAG_A))) <= 40,
          str(pixel(p, 1500, 600)))
    ws.vendor("SetIdentify", {"displayName": "HDMI-2", "mode": "off"})
    time.sleep(0.3)
    p = snapshot(ws, "HDMI-2", os.path.join(work, "identify_off.ppm"))
    r = compare(p, expect_crop(1920, 0, 200, 100, TAG_A), step=4)
    check("Identify off: output back to exact video", r[0] == 0, f"mismatches={r[0]}")

    # Slice locking: a locked slice can't be dragged, nudged or deleted.
    ws.vendor("Select", {"sliceName": "Slice 1"})
    _, before = slice_by_name(state(ws), "Slice 1")
    r = ws.vendor("SetSliceLocked", {"sliceName": "Slice 1", "locked": True})
    _, s1l = slice_by_name(r["state"], "Slice 1")
    check("Lock Slice 1", s1l.get("locked") is True)
    ws.vendor("EditorDrag", {"view": "input", "fromX": 960, "fromY": 540, "toX": 1260, "toY": 640, "alt": True})
    ws.vendor("EditorDrag", {"view": "output", "fromX": 960, "fromY": 540, "toX": 1160, "toY": 640, "alt": True})
    for _ in range(3):
        ws.vendor("EditorKey", {"view": "output", "key": 0x01000014})  # Right
    ws.vendor("EditorKey", {"view": "input", "key": 0x01000007})  # Delete
    _, after = slice_by_name(state(ws), "Slice 1")
    check("Locked slice ignores drags, nudges and Delete", after["src"] == before["src"] and
          after["dst"] == before["dst"], f"{rl(after['src'])} {rl(after['dst'])}")
    ws.vendor("SetSliceLocked", {"sliceName": "Slice 1", "locked": False})
    ws.vendor("EditorKey", {"view": "output", "key": 0x01000014})  # Right
    _, unl = slice_by_name(state(ws), "Slice 1")
    check("Unlocked slice can be edited again", rl(unl["dst"])[0] == rl(before["dst"])[0] + 1, str(rl(unl["dst"])))
    ws.vendor("EditorKey", {"view": "output", "key": 0x01000012})  # Left (back)
    ws.vendor("SetSliceLocked", {"sliceName": "Slice 1", "locked": True})  # stays locked: checked after restart
    time.sleep(0.8)

    # Layout presets: save, change, recall (running output must stay up).
    r = ws.vendor("SavePreset", {"name": "acceptance-A"})
    lp = ws.vendor("ListPresets")
    check("Save preset", r.get("ok") and any(p["name"] == "acceptance-A" for p in lp.get("presets", []))
          and lp.get("current") == "acceptance-A", str(lp.get("current")))
    ws.vendor("Select", {"sliceName": "Slice 2"})
    for _ in range(5):
        ws.vendor("EditorKey", {"view": "output", "key": 0x01000014})  # Right
    _, moved = slice_by_name(state(ws), "Slice 2")
    r = ws.vendor("LoadPreset", {"name": "acceptance-A"})
    st = state(ws)
    _, back = slice_by_name(st, "Slice 2")
    check("Recall preset restores the saved layout", rl(moved["dst"])[0] == rl(s2b["dst"])[0] + 5
          and back["dst"] == s2b["dst"], f"moved {rl(moved['dst'])} -> recalled {rl(back['dst'])}")
    check("Recall keeps running outputs on displays the preset still uses",
          disp(st, "HDMI-2")["output"] == "window", disp(st, "HDMI-2")["output"])
    r = ws.vendor("LoadPreset", {"name": "no-such-preset"})
    check("Recalling a missing preset reports an error", not r.get("ok"), r.get("error", ""))
    r = ws.vendor("DeletePreset", {"name": "acceptance-A"})
    lp = ws.vendor("ListPresets")
    check("Delete preset", r.get("ok") and not any(p["name"] == "acceptance-A" for p in lp.get("presets", [])))

    # Editor screenshot rendered by the plugin (no Screen Recording permission needed).
    ws.vendor("Select", {"sliceName": "Slice 2"})
    time.sleep(0.5)
    r = ws.vendor("EditorCapture", {"path": os.path.join(work, "editor_slice2.png")})
    check("Editor capture saved", r.get("ok"), f"{r.get('width')}x{r.get('height')}")
    ws.vendor("Select", {"sliceName": "Slice 1"})
    time.sleep(0.5)
    ws.vendor("EditorCapture", {"path": os.path.join(work, "editor_slice1.png")})

    log("\n== 5  Scene switch with transition; stop one output ==")
    ws.call("SetCurrentProgramScene", {"sceneName": "Scene B"})
    time.sleep(0.75)
    m1 = snapshot(ws, "HDMI-1", os.path.join(work, "step5_mid_hdmi1.ppm"))
    m2 = snapshot(ws, "HDMI-2", os.path.join(work, "step5_mid_hdmi2.ppm"))
    pa1, pa2 = pixel(m1, 10, 10), pixel(m2, 400, 400)
    exp_a1, exp_b1 = pattern_pixel(110, 60, TAG_A), pattern_pixel(110, 60, TAG_B)
    check("Mid-transition HDMI-1 is a blend of Scene A and Scene B",
          between(pa1[2], exp_a1[2], exp_b1[2]) and abs(pa1[0] - exp_a1[0]) <= 2,
          f"B={pa1[2]} (A={exp_a1[2]}, B={exp_b1[2]})")
    exp_a2, exp_b2 = pattern_pixel(1920 + 200, 300, TAG_A), pattern_pixel(1920 + 200, 300, TAG_B)
    check("Mid-transition HDMI-2 is a blend of Scene A and Scene B",
          between(pa2[2], exp_a2[2], exp_b2[2]), f"B={pa2[2]} (A={exp_a2[2]}, B={exp_b2[2]})")
    time.sleep(1.5)
    a = snapshot(ws, "HDMI-1", os.path.join(work, "step5_hdmi1.ppm"))
    b = snapshot(ws, "HDMI-2", os.path.join(work, "step5_hdmi2.ppm"))
    r = compare(a, expect_crop(100, 50, 0, 0, TAG_B))
    check("After transition HDMI-1 follows Program (Scene B)", r[0] == 0, f"mismatches={r[0]}")
    r = compare(b, expect_crop(1920, 0, 200, 100, TAG_B))
    check("After transition HDMI-2 follows Program (Scene B)", r[0] == 0, f"mismatches={r[0]}")

    ws.vendor("StopOutput", {"displayName": "HDMI-1"})
    time.sleep(0.5)
    st = state(ws)
    check("Stopping HDMI-1 leaves HDMI-2 running",
          disp(st, "HDMI-1")["output"] == "none" and disp(st, "HDMI-2")["output"] == "window",
          f"HDMI-1={disp(st, 'HDMI-1')['output']} HDMI-2={disp(st, 'HDMI-2')['output']}")
    ws.call("SetCurrentProgramScene", {"sceneName": "Scene A"})
    time.sleep(2.0)
    b = snapshot(ws, "HDMI-2", os.path.join(work, "step5_after_stop_hdmi2.ppm"))
    r = compare(b, expect_crop(1920, 0, 200, 100, TAG_A), step=4)
    check("HDMI-2 still follows Program after HDMI-1 stopped", r[0] == 0, f"mismatches={r[0]}")

    # Displays used by step 6.
    st = state(ws)
    builtin = next((s for s in st["screens"] if s["builtin"]), st["screens"][0])
    doc = {k: v for k, v in st.items() if k in ("format", "version", "restoreOutputs", "displays")}
    for d in doc["displays"]:
        for k in ("status", "statusText", "canFullscreen", "output"):
            d.pop(k, None)
    doc["displays"].append({"id": "missing", "name": "Missing HDMI", "kind": "physical", "width": 1920,
                            "height": 1080, "outputWanted": True,
                            "identity": {"uuid": "00000000-DEAD-BEEF-0000-000000000001", "vendor": 4660,
                                         "model": 22136, "serial": 42, "name": "Projector (unplugged)"},
                            "slices": [{"id": "ms", "name": "Missing slice", "color": "#e71d73",
                                        "src": R(0, 0, 1920, 1080), "dst": R(0, 0, 1920, 1080)}]})
    doc["displays"].append({"id": "ambig", "name": "Twin Display", "kind": "physical", "width": builtin["pixelWidth"],
                            "height": builtin["pixelHeight"], "outputWanted": True,
                            "identity": {"uuid": "00000000-AAAA-BBBB-0000-000000000002",
                                         "vendor": builtin["vendor"], "model": builtin["model"], "serial": 0,
                                         "name": "Same model, unknown unit"},
                            "slices": []})
    if fullscreen_builtin:
        doc["displays"].append({"id": "builtin", "name": "Built-in", "kind": "physical",
                                "width": builtin["pixelWidth"], "height": builtin["pixelHeight"],
                                "identity": {"uuid": builtin["uuid"], "vendor": builtin["vendor"],
                                             "model": builtin["model"], "serial": builtin["serial"],
                                             "name": builtin["name"]},
                                "slices": [{"id": "bs", "name": "Whole program", "color": "#2ec4b6",
                                            "src": R(0, 0, 3840, 1080),
                                            "dst": R(0, 0, builtin["pixelWidth"], builtin["pixelWidth"] * 1080 // 3840)}]})
    ws.vendor("SetState", {"state": doc})
    ws.vendor("OpenWindow", {"displayName": "HDMI-2"})
    st = state(ws)
    check("Missing display reports 'disconnected'", disp(st, "Missing HDMI")["status"] == "disconnected",
          disp(st, "Missing HDMI")["statusText"])
    check("Same-model display without serial/UUID reports 'ambiguous'", disp(st, "Twin Display")["status"] == "ambiguous",
          disp(st, "Twin Display")["statusText"])
    r = ws.vendor("StartFullscreen", {"displayName": "Missing HDMI"})
    check("StartFullscreen refused on disconnected display", not r.get("ok"), r.get("error", ""))
    r = ws.vendor("StartFullscreen", {"displayName": "Twin Display"})
    check("StartFullscreen refused on ambiguous display", not r.get("ok"), r.get("error", ""))

    # Test pattern on all displays, then clear all.
    r = ws.vendor("SetIdentify", {"mode": "pattern", "all": True})
    check("Test pattern on all displays",
          all(d["identify"] == "pattern" for d in r["state"]["displays"]), str([d["identify"] for d in r["state"]["displays"]]))
    time.sleep(0.3)
    a = snapshot(ws, "HDMI-1", os.path.join(work, "all_pattern_hdmi1.ppm"))
    check("HDMI-1 shows the test pattern edge", max(abs(x - y) for x, y in zip(pixel(a, 0, 0), (255, 40, 40))) <= 3,
          str(pixel(a, 0, 0)))
    r = ws.vendor("SetIdentify", {"mode": "clear"})
    check("Clear all patterns", all(d["identify"] == "off" for d in r["state"]["displays"]))

    # OBS dock: one live tile per display.
    time.sleep(0.5)  # the dock coalesces bursts of changes (50 ms)
    st = state(ws)
    r = ws.vendor("DockCapture", {"path": os.path.join(work, "dock.png")})
    check("OBS dock shows one live tile per display", r.get("ok") and r.get("tiles") == len(st["displays"]),
          f"tiles={r.get('tiles')} displays={len(st['displays'])} visible={r.get('visible')}")

    ws.vendor("SetIdentify", {"displayName": "HDMI-2", "mode": "pattern"})  # must NOT survive a restart
    if fullscreen_builtin:
        r = ws.vendor("StartFullscreen", {"displayName": "Built-in"})
        check("Borderless fullscreen opens on the built-in display", r.get("ok"), r.get("error", ""))
        time.sleep(3.0)
        b = disp(state(ws), "Built-in")
        check("Fullscreen output stays up (placement watchdog is satisfied)", b["output"] == "fullscreen",
              f"output={b['output']}")
        snap = snapshot(ws, "Built-in", os.path.join(work, "fullscreen_builtin_snapshot.ppm"))
        _ = snap
    return s1b, s2b


def phase_restart(ws, s1b, s2b, fullscreen_builtin):
    log("\n== 6  After restart: persistence and missing displays ==")
    time.sleep(3.0)  # restore runs 1.5 s after FINISHED_LOADING
    st = state(ws)
    _, s1 = slice_by_name(st, "Slice 1")
    _, s2 = slice_by_name(st, "Slice 2")
    check("Slice 1 persisted (src+dst)", s1["src"] == s1b["src"] and s1["dst"] == s1b["dst"], f"{s1['src']} {s1['dst']}")
    check("Slice 1 lock persisted across restart", s1.get("locked") is True)
    check("Slice 2 persisted (src+dst)", s2["src"] == s2b["src"] and s2["dst"] == s2b["dst"], f"{s2['src']} {s2['dst']}")
    check("Display names / kinds persisted",
          [d["name"] for d in st["displays"]][:4] == ["HDMI-1", "HDMI-2", "Missing HDMI", "Twin Display"],
          str([d["name"] for d in st["displays"]]))
    check("Test pattern is never restored after restart (always starts off)",
          all(d.get("identify", "off") == "off" for d in st["displays"]))
    m = disp(st, "Missing HDMI")
    check("Missing display shown as disconnected after restart", m["status"] == "disconnected", m["statusText"])
    check("Missing display was NOT opened anywhere (wanted output held back)", m["output"] == "none"
          and m["outputWanted"] is True, f"output={m['output']} outputWanted={m['outputWanted']}")
    t = disp(st, "Twin Display")
    check("Ambiguous display not opened", t["status"] == "ambiguous" and t["output"] == "none",
          f"{t['status']} output={t['output']}")
    if fullscreen_builtin:
        b = disp(st, "Built-in")
        check("Fullscreen on exactly-identified built-in display restored after restart",
              b["output"] == "fullscreen", f"output={b['output']}")
        ws.vendor("StopOutput", {"displayName": "Built-in"})
        b = disp(state(ws), "Built-in")
        check("User stop clears restore flag", b["output"] == "none" and b["outputWanted"] is False,
              f"output={b['output']} outputWanted={b['outputWanted']}")
    fs = [d["name"] for d in st["displays"] if d["output"] == "fullscreen"]
    check("No fullscreen output on any unidentified display",
          all(n == "Built-in" for n in fs), str(fs))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--obs-app", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--home", default=None, help="isolated OBS home (default: <work>/obs-home)")
    ap.add_argument("--plugin", default=DEFAULT_PLUGIN, help="plugin bundle to test")
    ap.add_argument("--fullscreen-builtin", action="store_true",
                    help="also open a real fullscreen output on the built-in display for a few seconds")
    args = ap.parse_args()
    os.makedirs(args.work, exist_ok=True)
    logfile = os.path.join(args.work, "obs-stdout.log")

    set_test_home(args.home or os.path.join(args.work, "obs-home"))
    install_plugin(args.plugin)
    reset_plugin_state()
    password = secrets.token_urlsafe(24)
    seed_config(args.port, password)
    proc = launch_obs(args.obs_app, logfile)
    try:
        ws = obsws.connect(port=args.port, password=password, retries=300)
        log(f"Connected to OBS {ws.call('GetVersion')['obsVersion']}")
        setup_obs(ws, args.work)
        s1b, s2b = phase_mapping(ws, args.work, args.fullscreen_builtin)
        ws.close()
    finally:
        log("\nQuitting OBS…")
        quit_obs(proc)

    log("Restarting OBS…")
    proc = launch_obs(args.obs_app, logfile)
    try:
        ws = obsws.connect(port=args.port, password=password, retries=300)
        phase_restart(ws, s1b, s2b, args.fullscreen_builtin)
        ws.close()
    finally:
        quit_obs(proc)

    passed = sum(1 for _, ok, _ in RESULTS if ok)
    log(f"\n{passed}/{len(RESULTS)} checks passed")
    with open(os.path.join(args.work, "results.json"), "w") as f:
        json.dump([{"check": n, "pass": ok, "detail": d} for n, ok, d in RESULTS], f, indent=2)
    sys.exit(0 if passed == len(RESULTS) else 1)


if __name__ == "__main__":
    main()
