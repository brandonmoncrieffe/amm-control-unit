#!/usr/bin/env python3
"""Tests for CSV-to-firmware calibration generation."""

from __future__ import annotations

import csv
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from generate_linear_actuator_calibration import (
    CalibrationError,
    discover_calibrations,
    load_calibration,
    render_source,
)


FIELDS = [
    "timestamp_utc",
    "servo_id",
    "gpio",
    "sweep_direction",
    "commanded_angle_deg",
    "measured_distance_mm",
]


def write_csv(path: Path, rows: list[dict[str, object]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(rows)


def interpolate_reference(points, requested_mm: float) -> tuple[float, int]:
    clamped = min(max(requested_mm, points[0].length_mm), points[-1].length_mm)
    for low, high in zip(points, points[1:]):
        if clamped <= high.length_mm:
            fraction = (clamped - low.length_mm) / (
                high.length_mm - low.length_mm
            )
            angle = low.angle_deg + fraction * (high.angle_deg - low.angle_deg)
            return clamped, round(angle)
    return clamped, round(points[-1].angle_deg)


class GeneratorTests(unittest.TestCase):
    def test_discovers_only_existing_servos_and_sorts_by_length(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            write_csv(
                root / "servo_1.csv",
                [
                    {"servo_id": 1, "sweep_direction": "outward", "commanded_angle_deg": 180, "measured_distance_mm": 0},
                    {"servo_id": 1, "sweep_direction": "outward", "commanded_angle_deg": 100, "measured_distance_mm": 10},
                    {"servo_id": 1, "sweep_direction": "outward", "commanded_angle_deg": 0, "measured_distance_mm": 24},
                    {"servo_id": 1, "sweep_direction": "return", "commanded_angle_deg": 100, "measured_distance_mm": 11},
                ],
            )
            calibrations = discover_calibrations(root)
            self.assertEqual(list(calibrations), [1])
            self.assertEqual(
                [point.length_mm for point in calibrations[1]], [0.0, 10.0, 24.0]
            )
            source = render_source(calibrations)
            self.assertIn("s_servo_1_points", source)
            self.assertIn("[1] = {.points = NULL", source)

    def test_current_repository_curve_interpolates_expected_angle(self) -> None:
        repo_root = Path(__file__).resolve().parents[1]
        points = load_calibration(repo_root / "calibration/servo_1.csv", 1)
        self.assertEqual(interpolate_reference(points, 0.0), (0.0, 180))
        self.assertEqual(interpolate_reference(points, 5.0), (5.0, 140))
        self.assertEqual(interpolate_reference(points, 12.0), (12.0, 73))
        self.assertEqual(interpolate_reference(points, 24.0), (24.0, 0))
        self.assertEqual(interpolate_reference(points, -1.0), (0.0, 180))
        self.assertEqual(interpolate_reference(points, 30.0), (24.0, 0))

    def test_trims_terminal_plateau_toward_varying_region(self) -> None:
        repo_root = Path(__file__).resolve().parents[1]
        points = load_calibration(repo_root / "calibration/servo_2.csv", 2)
        self.assertEqual(points[-1].length_mm, 25.0)
        self.assertEqual(points[-1].angle_deg, 160.0)
        self.assertEqual(interpolate_reference(points, 6.5), (6.5, 43))

    def test_rejects_invalid_calibrations(self) -> None:
        cases = {
            "mismatched servo": [
                (2, 180, 0),
                (2, 0, 10),
            ],
            "duplicate distance": [
                (1, 180, 0),
                (1, 90, 0),
            ],
            "duplicate angle": [
                (1, 180, 0),
                (1, 180, 10),
            ],
            "nonmonotonic distance": [
                (1, 180, 0),
                (1, 90, 10),
                (1, 0, 5),
            ],
            "invalid angle": [
                (1, 181, 0),
                (1, 0, 10),
            ],
        }
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name, values in cases.items():
                with self.subTest(name=name):
                    rows = [
                        {
                            "servo_id": servo_id,
                            "sweep_direction": "outward",
                            "commanded_angle_deg": angle,
                            "measured_distance_mm": distance,
                        }
                        for servo_id, angle, distance in values
                    ]
                    path = root / "servo_1.csv"
                    write_csv(path, rows)
                    with self.assertRaises(CalibrationError):
                        load_calibration(path, 1)

    def test_rejects_malformed_csv(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "servo_1.csv"
            path.write_text("servo_id,commanded_angle_deg\n1,180\n", encoding="utf-8")
            with self.assertRaises(CalibrationError):
                load_calibration(path, 1)


if __name__ == "__main__":
    unittest.main()
