#!/usr/bin/env python3
"""Show-readiness soak test for the Output Mapping plugin.

Runs a real OBS for a long period at 60 fps with three mapped outputs and
keeps changing things the way a busy show does, while checking that the
outputs stay pixel-exact and that OBS stays healthy:

  every  3 s  scene change with a 600 ms fade, then a pixel-exact check of
              HDMI-1 against the expected scene and layout
  every  7 s  delay change on HDMI-2 (0-800 ms, smooth ramp)
  every 11 s  slice nudge in the editor (LED wall)
  every 13 s  preset recall, alternating two layouts (A / B)
  every 17 s  identify on the LED wall: pattern -> outlines -> off
  every 30 s  OBS stats sample (memory, render time, skipped frames)

Pass criteria (printed and written to <work>/soak-report.md):
  * OBS never crashes or disconnects; clean quit at the end
  * every pixel check is exact
  * memory growth after warm-up < 50 MB
  * skipped render frames < 0.1 %
  * frame render time: mean < 8 ms, worst 30 s sample < 12 ms (60 fps budget 16.7 ms)

Usage:
  python3 tools/soak_test.py --obs-app ~/Library/Caches/obs-output-mapping/OBS.app --work /tmp/omsoak --minutes 10
"""

import argparse
import json
import os
import secrets
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import acceptance_test as at  # noqa: E402
import obsws  # noqa: E402
from testpattern import pattern_pixel  # noqa: E402

R = at.R


def layout(shift):
    """Layout A (shift 0) and B (shift 100): HDMI-1 crop moves by `shift`."""
    return {"format": "obs-output-mapping", "version": 1, "displays": [
        {"id": "hdmi-1", "name": "HDMI-1", "kind": "virtual", "width": 1920, "height": 1080, "slices": [
            {"id": "a", "name": "Left half", "color": "#ff9f1c",
             "src": R(shift, 0, 1920, 1080), "dst": R(0, 0, 1920, 1080)}]},
        {"id": "hdmi-2", "name": "HDMI-2", "kind": "virtual", "width": 1920, "height": 1080, "slices": [
            {"id": "b", "name": "Right half", "color": "#2ec4b6",
             "src": R(1920, 0, 1920, 1080), "dst": R(0, 0, 1920, 1080)}]},
        {"id": "led", "name": "LED Wall", "kind": "virtual", "width": 1280, "height": 720, "slices": [
            {"id": "c", "name": "Wide", "color": "#7b8cff", "src": R(0, 0, 3840, 1080), "dst": R(0, 0, 1280, 360)},
            {"id": "d", "name": "Logo corner", "color": "#e71d73", "src": R(3200, 700, 640, 360),
             "dst": R(640, 360, 640, 360)},
            {"id": "e", "name": "Detail", "color": "#a7e03c", "src": R(1600, 300, 640, 360),
             "dst": R(0, 360, 640, 360)}]},
    ]}


def stats(ws):
    s = ws.call("GetStats")
    return {"t": time.time(), "memMB": s["memoryUsage"], "cpu": s["cpuUsage"],
            "renderSkipped": s["renderSkippedFrames"], "renderTotal": s["renderTotalFrames"],
            "avgRenderMs": s["averageFrameRenderTime"], "fps": s["activeFps"]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--obs-app", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--minutes", type=float, default=10.0)
    ap.add_argument("--warmup", type=float, default=60.0, help="seconds before the memory baseline")
    args = ap.parse_args()
    os.makedirs(args.work, exist_ok=True)

    pw = secrets.token_urlsafe(24)
    at.seed_config(4455, pw)
    proc = at.launch_obs(args.obs_app, os.path.join(args.work, "soak-obs.log"))
    report = {"events": {}, "samples": [], "failures": [], "pixelChecks": 0}
    ev = report["events"]

    def fail(msg):
        report["failures"].append(f"{time.strftime('%H:%M:%S')} {msg}")
        print("  FAIL", msg, flush=True)

    clean_exit = False
    try:
        ws = obsws.connect(port=4455, password=pw, retries=90)
        at.setup_obs(ws, args.work)
        ws.call("SetVideoSettings", {"baseWidth": 3840, "baseHeight": 1080, "outputWidth": 1920,
                                     "outputHeight": 540, "fpsNumerator": 60, "fpsDenominator": 1})
        ws.call("SetCurrentSceneTransition", {"transitionName": "Fade"})
        ws.call("SetCurrentSceneTransitionDuration", {"transitionDuration": 600})

        # Two presets, A and B.
        ws.vendor("OpenEditor", {"width": 1500, "height": 860})
        for name, shift in (("Soak A", 0), ("Soak B", 100)):
            ws.vendor("SetState", {"state": layout(shift)})
            ws.vendor("SavePreset", {"name": name})
        ws.vendor("LoadPreset", {"name": "Soak A"})
        for n in ("HDMI-1", "HDMI-2", "LED Wall"):
            r = ws.vendor("OpenWindow", {"displayName": n})
            if not r.get("ok"):
                fail(f"could not open {n}: {r}")
        ws.vendor("Select", {"sliceName": "Detail"})

        scene, shift = "Scene A", 0
        ws.call("SetCurrentProgramScene", {"sceneName": scene})
        start = time.time()
        end = start + args.minutes * 60
        nxt = {"scene": start + 3, "delay": start + 7, "nudge": start + 11, "preset": start + 13,
               "identify": start + 17, "stats": start, "verify": None}
        ident_cycle = ["pattern", "outlines", "off"]
        ident_i = 0
        preset_b = False
        baseline = None
        print(f"Soak running for {args.minutes:.0f} min at 60 fps…", flush=True)

        while time.time() < end:
            now = time.time()
            if now >= nxt["scene"]:
                scene = "Scene B" if scene == "Scene A" else "Scene A"
                ws.call("SetCurrentProgramScene", {"sceneName": scene})
                ev["sceneChanges"] = ev.get("sceneChanges", 0) + 1
                nxt["scene"] = now + 3
                nxt["verify"] = now + 1.2  # after the 600 ms fade
            if nxt["verify"] and now >= nxt["verify"]:
                nxt["verify"] = None
                tag = at.TAG_A if scene == "Scene A" else at.TAG_B
                p = at.snapshot(ws, "HDMI-1", os.path.join(args.work, "soak-check.ppm"))
                res = at.compare(p, at.expect_crop(shift, 0, 0, 0, tag), step=8)
                report["pixelChecks"] += 1
                if res[0] != 0:
                    fail(f"HDMI-1 not exact ({scene}, shift {shift}): mismatches={res[0]} first={res[3]}")
            if now >= nxt["delay"]:
                d = secrets.choice([0, 100, 250, 400, 800, 50, 0])
                ws.vendor("SetDelay", {"displayName": "HDMI-2", "delayMs": d})
                ev["delayChanges"] = ev.get("delayChanges", 0) + 1
                nxt["delay"] = now + 7
            if now >= nxt["nudge"]:
                ws.vendor("Select", {"sliceName": "Detail"})
                key = secrets.choice([0x01000012, 0x01000014, 0x01000013, 0x01000015])
                ws.vendor("EditorKey", {"view": "output", "key": key})
                ev["nudges"] = ev.get("nudges", 0) + 1
                nxt["nudge"] = now + 11
            if now >= nxt["preset"]:
                preset_b = not preset_b
                r = ws.vendor("LoadPreset", {"name": "Soak B" if preset_b else "Soak A"})
                if not r.get("ok"):
                    fail(f"preset recall failed: {r}")
                shift = 100 if preset_b else 0
                st = r.get("state", {})
                open_n = sum(1 for d in st.get("displays", []) if d.get("output") == "window")
                if open_n != 3:
                    fail(f"preset recall closed outputs ({open_n}/3 open)")
                ev["presetRecalls"] = ev.get("presetRecalls", 0) + 1
                nxt["preset"] = now + 13
                if nxt["verify"] is None:
                    nxt["verify"] = now + 0.3
            if now >= nxt["identify"]:
                mode = ident_cycle[ident_i % 3]
                ident_i += 1
                ws.vendor("SetIdentify", {"displayName": "LED Wall", "mode": mode})
                ev["identifyToggles"] = ev.get("identifyToggles", 0) + 1
                nxt["identify"] = now + 17
            if now >= nxt["stats"]:
                s = stats(ws)
                report["samples"].append(s)
                if baseline is None and now - start >= args.warmup:
                    baseline = s
                nxt["stats"] = now + 30
                el = (now - start) / 60
                print(f"  {el:5.1f} min  mem {s['memMB']:.0f} MB  render {s['avgRenderMs']:.2f} ms  "
                      f"skipped {s['renderSkipped']}/{s['renderTotal']}  checks {report['pixelChecks']}  "
                      f"failures {len(report['failures'])}", flush=True)
            time.sleep(0.05)

        last = stats(ws)
        report["samples"].append(last)
        ws.vendor("SetIdentify", {"mode": "clear"})
        ws.vendor("StopAll")
        for n in ("Soak A", "Soak B"):
            ws.vendor("DeletePreset", {"name": n})
        ws.close()
    except Exception as e:  # crash / disconnect
        fail(f"OBS connection lost or error: {e!r}")
        last = baseline = None
    finally:
        clean_exit = at.quit_obs(proc)

    # ---- verdict ----
    checks = []
    def chk(name, ok, detail):
        checks.append((name, bool(ok), detail))
    chk("No crash or disconnect", not any("connection lost" in f for f in report["failures"]), "")
    chk("Every pixel check exact", report["pixelChecks"] > 0 and not any("not exact" in f for f in report["failures"]),
        f"{report['pixelChecks']} checks")
    chk("No other failures", not report["failures"], f"{len(report['failures'])} failure(s)")
    if baseline and last:
        growth = last["memMB"] - baseline["memMB"]
        chk("Memory growth after warm-up < 50 MB", growth < 50,
            f"{baseline['memMB']:.0f} → {last['memMB']:.0f} MB ({growth:+.1f})")
        sk = last["renderSkipped"] - baseline["renderSkipped"]
        tot = max(1, last["renderTotal"] - baseline["renderTotal"])
        chk("Skipped render frames < 0.1 %", sk / tot < 0.001, f"{sk} of {tot} ({100 * sk / tot:.3f} %)")
        rs = [x["avgRenderMs"] for x in report["samples"][1:]] or [last["avgRenderMs"]]
        mean, worst = sum(rs) / len(rs), max(rs)
        chk("Frame render time: mean < 8 ms, worst sample < 12 ms (budget 16.7 ms)",
            mean < 8 and worst < 12, f"mean {mean:.2f} ms, worst {worst:.2f} ms")
    else:
        chk("Stats collected", False, "no baseline/last sample")
    chk("OBS quit cleanly", clean_exit, "")

    passed = all(ok for _, ok, _ in checks)
    lines = [f"# Soak test report", "",
             f"Duration: {args.minutes:.0f} min at 60 fps, 3 outputs (2×1920×1080, 1×1280×720)", "",
             "| Check | Result | Detail |", "|---|---|---|"]
    lines += [f"| {n} | {'PASS' if ok else 'FAIL'} | {d} |" for n, ok, d in checks]
    lines += ["", "Events: " + ", ".join(f"{k} {v}" for k, v in ev.items()),
              "", "Failures:" if report["failures"] else "Failures: none"]
    lines += [f"- {f}" for f in report["failures"]]
    lines += ["", f"**{'SHOW READY: all checks passed' if passed else 'NOT READY: see failures'}**"]
    out = "\n".join(lines)
    print("\n" + out)
    with open(os.path.join(args.work, "soak-report.md"), "w") as f:
        f.write(out + "\n")
    with open(os.path.join(args.work, "soak-report.json"), "w") as f:
        json.dump({"checks": checks, **report}, f, indent=2)
    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
