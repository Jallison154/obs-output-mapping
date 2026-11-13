# Soak test report

Duration: 10 min at 60 fps, 3 outputs (2×1920×1080, 1×1280×720)

| Check | Result | Detail |
|---|---|---|
| No crash or disconnect | PASS |  |
| Every pixel check exact | PASS | 220 checks |
| No other failures | PASS | 0 failure(s) |
| Memory growth after warm-up < 50 MB | PASS | 196 → 206 MB (+9.1) |
| Skipped render frames < 0.1 % | PASS | 4 of 32397 (0.012 %) |
| Average frame render time < 8 ms (worst sample) | FAIL | 8.52 ms |
| OBS quit cleanly | PASS |  |

Events: sceneChanges 198, delayChanges 85, nudges 54, presetRecalls 46, identifyToggles 35

Failures: none

**NOT READY: see failures**
