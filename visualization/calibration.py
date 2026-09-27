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
