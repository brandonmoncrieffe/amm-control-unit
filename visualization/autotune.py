"""Background auto-tuning loop: nudge each cavity's piston toward whatever
frequency is currently loudest in its slice of the incoming spectrum.

Physics (validated earlier against the project's own depth_for_f() derivation):
f0 = (c / 2*pi) * sqrt(A / (V * L_eff)), and V grows with piston depth, so f0
falls as depth increases. That monotonic relationship is all this script
relies on — it does not need the neck area/length constants, because it
never computes an absolute target depth. Instead, each cycle it only asks
"is the locally loudest frequency above or below what this cavity is
currently tuned to?" and takes one small step in the matching direction:
higher peak frequency -> shallower (less depth), lower peak frequency ->
deeper (more depth).

This is a peak-tracking hill-climb, not a closed-form solve, and it is
intentionally slow and coarse. The servos are SG90s: cheap, not rated for
continuous duty, prone to backlash, and audible when they move (which then
contaminates the very spectrum this script is reading). So the loop:

- moves at most one servo per cycle (round-robin across cavities), never all
  four at once;
- only moves a servo when the peak in its zone is both meaningfully loud
  (--min-level-dbfs) and meaningfully off-target (--tolerance-hz) — most
  cycles should do nothing;
- takes one small step (--step-deg) per cycle rather than jumping straight
  to a computed target, so it can never overshoot far or oscillate wildly;
- enforces a hard cap on moves per servo per minute (--max-moves-per-minute)
  independent of the cycle interval, as a second line of defense; and
- refuses to move a servo at all without calibration data for it (no
  calibration/servo_N.csv means we don't even know which physical direction
  "more depth" is, let alone how many degrees that is), and refuses to
  command an angle outside that calibration's own exercised range.

Defaults to --dry-run: it logs every decision it would make without ever
calling /servo. Pass --enable to actually move servos.
"""

from __future__ import annotations

import argparse
import logging
import time
from dataclasses import dataclass

from visualization.calibration import load_calibrations
from visualization.esp32_client import DEFAULT_BASE_URL, Esp32Client

logger = logging.getLogger("autotune")

SERVO_COUNT = 4
STEP_DEPTH_FALLBACK_MM = 0.5  # used only if a calibration curve is degenerate


@dataclass(frozen=True)
class Zone:
    cavity_id: int
    target_hz: float
    low_hz: float
    high_hz: float


def build_zones(target_hz: list[float], top_hz: float) -> list[Zone]:
    """One frequency zone per cavity, split at the midpoints between the
    firmware's configured target bands, so cavities never chase the same
    peak."""
    ordered = sorted(range(len(target_hz)), key=lambda i: target_hz[i])
    bounds = [0.0]
    for a, b in zip(ordered, ordered[1:]):
        bounds.append((target_hz[a] + target_hz[b]) / 2.0)
    bounds.append(top_hz)

    zones = []
    for rank, cavity_index in enumerate(ordered):
        zones.append(
            Zone(
                cavity_id=cavity_index + 1,
                target_hz=target_hz[cavity_index],
                low_hz=bounds[rank],
                high_hz=bounds[rank + 1],
            )
        )
    return zones


def find_peak(freqs_hz: list[float], dbfs: list[float], low_hz: float, high_hz: float) -> tuple[float, float] | None:
    best: tuple[float, float] | None = None
    for hz, level in zip(freqs_hz, dbfs):
        if hz < low_hz or hz > high_hz:
            continue
        if best is None or level > best[1]:
            best = (hz, level)
    return best


class RateLimiter:
    """Caps how many moves a servo can make in a trailing 60-second window,
    independent of (and in addition to) the cycle interval."""

    def __init__(self, max_moves_per_minute: int) -> None:
        self._max_moves = max_moves_per_minute
        self._move_times: dict[int, list[float]] = {}

    def allow(self, servo_id: int, now: float) -> bool:
        history = self._move_times.setdefault(servo_id, [])
        cutoff = now - 60.0
        while history and history[0] < cutoff:
            history.pop(0)
        return len(history) < self._max_moves

    def record(self, servo_id: int, now: float) -> None:
        self._move_times.setdefault(servo_id, []).append(now)


def run(args: argparse.Namespace) -> None:
    client = Esp32Client(args.esp32_host)
    calibrations = load_calibrations(SERVO_COUNT)
    limiter = RateLimiter(args.max_moves_per_minute)

    missing = sorted(set(range(1, SERVO_COUNT + 1)) - calibrations.keys())
    if missing:
        logger.warning(
            "No calibration data for cavity/cavities %s — those will be "
            "logged as skipped every cycle, never moved. Run "
            "tools/servo_calibration.py for them first.",
            missing,
        )
    if not args.enable:
        logger.warning("DRY RUN: no servo will actually move. Pass --enable to arm it.")
    else:
        logger.warning(
            "ENABLED: this will physically move servos over WiFi. Ctrl-C to stop."
        )

    cavity_order = list(range(1, SERVO_COUNT + 1))
    rotation_index = 0

    while True:
        state = client.fetch_state()
        spectrum = client.fetch_spectrum()
        if not state.connected or state.audio is None or spectrum is None:
            logger.info("ESP32 unreachable or not yet producing measurements; skipping cycle.")
            time.sleep(args.interval)
            continue

        zones = build_zones(state.audio.targets_hz, args.search_ceiling_hz)
        freqs_hz = [bin_index * spectrum.bin_width_hz for bin_index in range(len(spectrum.dbfs))]

        cavity_id = cavity_order[rotation_index % len(cavity_order)]
        rotation_index += 1
        zone = next(z for z in zones if z.cavity_id == cavity_id)

        calibration = calibrations.get(cavity_id)
        if calibration is None:
            logger.info("cavity %d: skipped (no calibration data)", cavity_id)
            time.sleep(args.interval)
            continue

        peak = find_peak(freqs_hz, spectrum.dbfs, zone.low_hz, zone.high_hz)
        if peak is None:
            logger.info("cavity %d: no spectrum bins in zone %.0f-%.0fhz", cavity_id, zone.low_hz, zone.high_hz)
            time.sleep(args.interval)
            continue
        peak_hz, peak_dbfs = peak

        if peak_dbfs < args.min_level_dbfs:
            logger.info(
                "cavity %d: quiet (peak %.0fhz at %.1fdb < floor %.1fdb) — not chasing noise",
                cavity_id, peak_hz, peak_dbfs, args.min_level_dbfs,
            )
            time.sleep(args.interval)
            continue

        current_target_hz = zone.target_hz
        if abs(peak_hz - current_target_hz) <= args.tolerance_hz:
            logger.info(
                "cavity %d: on target (peak %.0fhz within %.0fhz of %.0fhz)",
                cavity_id, peak_hz, args.tolerance_hz, current_target_hz,
            )
            time.sleep(args.interval)
            continue

        servo = next((s for s in state.servos if s.id == cavity_id), None)
        if servo is None:
            time.sleep(args.interval)
            continue

        current_depth_mm = calibration.depth_mm(servo.angle_deg)
        # Higher peak frequency needs a shallower cavity (less depth);
        # lower peak frequency needs a deeper one. See module docstring.
        direction = -1.0 if peak_hz > current_target_hz else 1.0
        target_depth_mm = current_depth_mm + direction * args.step_mm
        target_depth_mm = max(0.0, min(calibration.max_depth_mm, target_depth_mm))
        target_angle = calibration.angle_for_depth(target_depth_mm)
        angle_min, angle_max = calibration.angle_range_deg
        target_angle = max(angle_min, min(angle_max, target_angle))
        target_angle_int = int(round(target_angle))

        if target_angle_int == servo.angle_deg:
            logger.info(
                "cavity %d: wants to move toward %.0fhz but is already at its "
                "calibrated angle limit (%d deg)",
                cavity_id, peak_hz, servo.angle_deg,
            )
            time.sleep(args.interval)
            continue

        now = time.monotonic()
        if not limiter.allow(cavity_id, now):
            logger.info(
                "cavity %d: wants to move toward %.0fhz but hit the %d "
                "moves/minute cap — waiting",
                cavity_id, peak_hz, args.max_moves_per_minute,
            )
            time.sleep(args.interval)
            continue

        logger.info(
            "cavity %d: peak %.0fhz vs target %.0fhz -> %s %d deg to %d deg "
            "(depth %.1fmm -> %.1fmm)%s",
            cavity_id, peak_hz, current_target_hz,
            "stepping" if args.enable else "would step",
            servo.angle_deg, target_angle_int,
            current_depth_mm, target_depth_mm,
            "" if args.enable else " [dry run]",
        )

        if args.enable:
            applied = client.set_servo_angle(cavity_id, target_angle_int)
            if applied is None:
                logger.warning("cavity %d: /servo request failed", cavity_id)
            else:
                limiter.record(cavity_id, now)

        time.sleep(args.interval)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--esp32-host", default=DEFAULT_BASE_URL)
    parser.add_argument("--enable", action="store_true", help="actually move servos (default: dry run / log only)")
    parser.add_argument("--interval", type=float, default=8.0, help="seconds between cycles; one cavity is considered per cycle (default: 8)")
    parser.add_argument("--step-mm", type=float, default=1.0, help="depth change per nudge, in mm (default: 1.0)")
    parser.add_argument("--tolerance-hz", type=float, default=15.0, help="don't move if the peak is already this close to the target (default: 15)")
    parser.add_argument("--min-level-dbfs", type=float, default=-65.0, help="ignore peaks quieter than this — don't chase the noise floor (default: -65)")
    parser.add_argument("--search-ceiling-hz", type=float, default=800.0, help="top edge of the highest cavity's search zone (default: 800)")
    parser.add_argument("--max-moves-per-minute", type=int, default=3, help="hard cap per servo, independent of --interval (default: 3)")
    return parser.parse_args()


def main() -> None:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s", datefmt="%H:%M:%S")
    args = parse_args()
    try:
        run(args)
    except KeyboardInterrupt:
        logger.info("Stopped.")


if __name__ == "__main__":
    main()
