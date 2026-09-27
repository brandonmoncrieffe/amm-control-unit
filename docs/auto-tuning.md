# Background Auto-Tuning

`visualization/autotune.py` is a standalone script (independent of the
dashboard) that nudges each cavity's piston toward whatever frequency is
currently loudest in its slice of the incoming spectrum. It is opt-in and
off by default.

## Why it can get away with not knowing the neck geometry

`f0 = (c / 2*pi) * sqrt(A / (V * L_eff))` — increasing a cavity's volume `V`
(more piston depth) lowers `f0`. The script only ever asks "is the loudest
frequency in this cavity's zone currently above or below what it's tuned
to?" and takes one small step in the matching direction. It never solves for
an absolute target depth, so it doesn't need the neck area or effective
length constants — only that monotonic relationship, which is already
validated in this repo's own `depth_for_f()` derivation.

Each of the 4 configured target bands (73/145/213/395 Hz) gets a frequency
"zone" split at the midpoints between neighboring targets, so two cavities
never chase the same peak.

## Why it moves slowly and rarely

The servos are SG90s: cheap, not rated for continuous duty cycling, prone to
backlash, and audible when they move — which then contaminates the very
spectrum this script is reading. So by design it:

- considers one cavity per cycle, round-robin, never all four at once;
- only acts when the zone's peak is both loud enough (`--min-level-dbfs`,
  default -65 dBFS) and off-target enough (`--tolerance-hz`, default 15 Hz)
  — most cycles do nothing;
- takes one small depth step per cycle (`--step-mm`, default 1.0 mm) rather
  than jumping to a computed target, so it can't overshoot or oscillate far
  in one move;
- enforces a hard cap on moves per servo per minute (`--max-moves-per-minute`,
  default 3), independent of the cycle interval, as a second line of
  defense; and
- refuses to move a cavity at all without calibration data for it — no
  `calibration/servo_N.csv` means the script doesn't know which physical
  direction "more depth" even is, so it logs the cavity as skipped instead
  of guessing.

## Running it

Defaults to dry run — it logs every decision without ever calling `/servo`:

```sh
python3 -m visualization.autotune --esp32-host http://192.168.4.1
```

Watch the log for a cycle or two per cavity before arming it, then:

```sh
python3 -m visualization.autotune --esp32-host http://192.168.4.1 --enable
```

Run it alongside the dashboard (`python3 -m visualization.server`) — they
poll the same read-only endpoints independently and don't interfere; only
`autotune.py` ever calls the mutating `POST /servo` endpoint.

## Known gaps

- Needs `calibration/servo_N.csv` for a cavity before it will touch it. None
  are checked into this repository yet — run `tools/servo_calibration.py`
  first.
- The peak-tracking approach assumes the microphone can usefully distinguish
  "this cavity's zone got louder because of ambient noise" from "because a
  servo just moved and made noise." No settling-time debounce beyond the
  cycle interval is implemented yet — keep `--interval` comfortably longer
  than your mechanism's settle time (see `docs/acoustic-demo.md`'s
  experimental-controls section).
- This tunes each cavity toward the loudest thing in its own zone in
  isolation. It does not implement the multi-resonator transfer-matrix
  model discussed earlier for predicting the combined system response —
  that remains a separate, not-yet-built piece.
