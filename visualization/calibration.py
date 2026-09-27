"""Angle-to-depth interpolation built from tools/servo_calibration.py output.

No calibration CSVs are checked into this repository yet — run
tools/servo_calibration.py per servo first. Until that exists for a given
servo, callers must fall back to displaying its raw commanded angle instead
of a depth in millimetres.
"""

from __future__ import annotations

import bisect
import csv
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
CALIBRATION_DIR = REPOSITORY_ROOT / "calibration"


class DepthCalibration:
    """Piecewise-linear angle (degrees) -> travel (mm) lookup for one servo."""

    def __init__(self, angles_deg: list[float], distances_mm: list[float]) -> None:
        paired = sorted(zip(angles_deg, distances_mm))
        self._angles = [angle for angle, _ in paired]
        self._distances = [distance for _, distance in paired]

    @property
    def max_depth_mm(self) -> float:
        return max(self._distances)

    @property
    def angle_range_deg(self) -> tuple[float, float]:
        """(min, max) commanded angle actually exercised during calibration.

        auto-tuning must stay within this range rather than the servo's full
        0-180 travel: it is the only range this calibration curve — and thus
        the only range angle_for_depth() — was ever validated against.
        """
        return (self._angles[0], self._angles[-1])

    def depth_mm(self, angle_deg: float) -> float:
        if angle_deg <= self._angles[0]:
            return self._distances[0]
        if angle_deg >= self._angles[-1]:
            return self._distances[-1]
        index = bisect.bisect_right(self._angles, angle_deg) - 1
        angle_low, angle_high = self._angles[index], self._angles[index + 1]
        distance_low, distance_high = self._distances[index], self._distances[index + 1]
        if angle_high == angle_low:
            return distance_low
        fraction = (angle_deg - angle_low) / (angle_high - angle_low)
        return distance_low + fraction * (distance_high - distance_low)

    def angle_for_depth(self, depth_mm: float) -> float:
        """Inverse of depth_mm(): nearest calibrated angle for a target depth.

        Re-sorts the same measured points by depth so this works regardless
        of whether this servo's mechanism increases or decreases depth as
        angle increases — that direction is a property of the physical
        linkage, not something to assume.
        """
        paired = sorted(zip(self._distances, self._angles))
        depths = [depth for depth, _ in paired]
        angles = [angle for _, angle in paired]
        if depth_mm <= depths[0]:
            return angles[0]
        if depth_mm >= depths[-1]:
            return angles[-1]
        index = bisect.bisect_right(depths, depth_mm) - 1
        depth_low, depth_high = depths[index], depths[index + 1]
        angle_low, angle_high = angles[index], angles[index + 1]
        if depth_high == depth_low:
            return angle_low
        fraction = (depth_mm - depth_low) / (depth_high - depth_low)
        return angle_low + fraction * (angle_high - angle_low)


def load_calibrations(servo_count: int) -> dict[int, DepthCalibration]:
    """Load calibration/servo_N.csv for each servo that has recorded data."""
    calibrations: dict[int, DepthCalibration] = {}
    for servo_id in range(1, servo_count + 1):
        csv_path = CALIBRATION_DIR / f"servo_{servo_id}.csv"
        if not csv_path.exists():
            continue

        angles: list[float] = []
        distances: list[float] = []
        with csv_path.open(newline="", encoding="utf-8") as handle:
            for row in csv.DictReader(handle):
                measured = (row.get("measured_distance_mm") or "").strip()
                if not measured:
                    continue
                angles.append(float(row["commanded_angle_deg"]))
                distances.append(float(measured))

        if angles:
            calibrations[servo_id] = DepthCalibration(angles, distances)

    return calibrations
