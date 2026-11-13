# Soak test report

Duration: 30 min at 60 fps, 3 outputs (2×1920×1080, 1×1280×720)

| Check | Result | Detail |
|---|---|---|
| No crash or disconnect | PASS |  |
| Every pixel check exact | PASS | 661 checks |
| No other failures | PASS | 0 failure(s) |
| Memory growth after warm-up < 50 MB | PASS | 309 → 213 MB (-95.8) |
| Skipped render frames < 0.1 % | PASS | 32 of 104397 (0.031 %) |
| Frame render time: mean < 8 ms, worst sample < 12 ms (budget 16.7 ms) | PASS | mean 4.95 ms, worst 8.57 ms |
| OBS quit cleanly | PASS |  |

Events: sceneChanges 594, delayChanges 256, nudges 163, presetRecalls 138, identifyToggles 105

Failures: none

**SHOW READY: all checks passed**
