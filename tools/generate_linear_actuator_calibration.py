#!/usr/bin/env python3
"""Generate embedded linear-actuator calibration tables from servo CSV files."""

from __future__ import annotations

import argparse
import csv
import math
from dataclasses import dataclass
from pathlib import Path


SERVO_COUNT = 4


class CalibrationError(ValueError):
    """Raised when an existing calibration file cannot be embedded safely."""


@dataclass(frozen=True)
class CalibrationPoint:
    length_mm: float
    angle_deg: float


def _parse_finite(value: str, field: str, path: Path, line: int) -> float:
    try:
        parsed = float(value)
    except ValueError as error:
        raise CalibrationError(
            f"{path}:{line}: {field} must be numeric, got {value!r}"
        ) from error
    if not math.isfinite(parsed):
        raise CalibrationError(f"{path}:{line}: {field} must be finite")
    return parsed


def load_calibration(path: Path, expected_servo_id: int) -> list[CalibrationPoint]:
    required_fields = {
        "servo_id",
        "sweep_direction",
        "commanded_angle_deg",
        "measured_distance_mm",
    }
    points: list[CalibrationPoint] = []

    try:
        handle = path.open(newline="", encoding="utf-8")
    except OSError as error:
        raise CalibrationError(f"Could not read {path}: {error}") from error

    with handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None:
            raise CalibrationError(f"{path}: missing CSV header")
        missing = required_fields.difference(reader.fieldnames)
        if missing:
            raise CalibrationError(
                f"{path}: missing required column(s): {', '.join(sorted(missing))}"
            )

        for line, row in enumerate(reader, start=2):
            servo_text = (row.get("servo_id") or "").strip()
            try:
                servo_id = int(servo_text)
            except ValueError as error:
                raise CalibrationError(
                    f"{path}:{line}: servo_id must be an integer"
                ) from error
            if servo_id != expected_servo_id:
                raise CalibrationError(
                    f"{path}:{line}: servo_id {servo_id} does not match "
                    f"servo_{expected_servo_id}.csv"
                )

            if (row.get("sweep_direction") or "").strip() != "outward":
                continue

            measured_text = (row.get("measured_distance_mm") or "").strip()
            if not measured_text:
                continue
            angle = _parse_finite(
                (row.get("commanded_angle_deg") or "").strip(),
                "commanded_angle_deg",
                path,
                line,
            )
            if not angle.is_integer() or not 0.0 <= angle <= 180.0:
                raise CalibrationError(
                    f"{path}:{line}: commanded_angle_deg must be an integer "
                    "within 0..180"
                )
            distance = _parse_finite(
                measured_text, "measured_distance_mm", path, line
            )
            points.append(CalibrationPoint(distance, angle))

    if len(points) < 2:
        raise CalibrationError(
            f"{path}: at least two outward measurements are required"
        )
    if len({point.angle_deg for point in points}) != len(points):
        raise CalibrationError(f"{path}: outward angles must be unique")

    by_angle = sorted(points, key=lambda point: point.angle_deg)
    while (
        len(by_angle) >= 2
        and by_angle[0].length_mm == by_angle[1].length_mm
    ):
        del by_angle[0]
    while (
        len(by_angle) >= 2
        and by_angle[-1].length_mm == by_angle[-2].length_mm
    ):
        by_angle.pop()
    if len(by_angle) < 2:
        raise CalibrationError(
            f"{path}: terminal distance plateau leaves fewer than two points"
        )
    if len({point.length_mm for point in by_angle}) != len(by_angle):
        raise CalibrationError(
            f"{path}: repeated distances are allowed only at terminal plateaus"
        )

    distance_steps = [
        second.length_mm - first.length_mm
        for first, second in zip(by_angle, by_angle[1:])
    ]
    increasing = all(step > 0.0 for step in distance_steps)
    decreasing = all(step < 0.0 for step in distance_steps)
    if not increasing and not decreasing:
        raise CalibrationError(
            f"{path}: outward distance must be strictly monotonic with angle"
        )

    return sorted(by_angle, key=lambda point: point.length_mm)


def discover_calibrations(calibration_dir: Path) -> dict[int, list[CalibrationPoint]]:
    calibrations: dict[int, list[CalibrationPoint]] = {}
    for servo_id in range(1, SERVO_COUNT + 1):
        path = calibration_dir / f"servo_{servo_id}.csv"
        if path.exists():
            calibrations[servo_id] = load_calibration(path, servo_id)
    return calibrations


def _c_float(value: float) -> str:
    return f"{value:.9g}f" if not value.is_integer() else f"{value:.1f}f"


def render_header() -> str:
    return """// Generated at build time. Do not edit.
#pragma once

#include "linear_actuator_internal.h"

extern const linear_actuator_calibration_t
    g_linear_actuator_calibrations[SERVO_COUNT];
"""


def render_source(calibrations: dict[int, list[CalibrationPoint]]) -> str:
    lines = [
        "// Generated at build time. Do not edit.",
        '#include "linear_actuator_calibration_data.h"',
        "",
    ]
    for servo_id, points in sorted(calibrations.items()):
        lines.append(
            f"static const linear_actuator_point_t s_servo_{servo_id}_points[] = {{"
        )
        for point in points:
            lines.append(
                "    {"
                f".length_mm = {_c_float(point.length_mm)}, "
                f".angle_deg = {_c_float(point.angle_deg)}"
                "},"
            )
        lines.extend(["};", ""])

    lines.append(
        "const linear_actuator_calibration_t "
        "g_linear_actuator_calibrations[SERVO_COUNT] = {"
    )
    for servo_id in range(1, SERVO_COUNT + 1):
        if servo_id in calibrations:
            lines.append(
                f"    [{servo_id - 1}] = {{"
                f".points = s_servo_{servo_id}_points, "
                f".point_count = sizeof(s_servo_{servo_id}_points) / "
                f"sizeof(s_servo_{servo_id}_points[0])"
                "},"
            )
        else:
            lines.append(f"    [{servo_id - 1}] = {{.points = NULL, .point_count = 0U}},")
    lines.extend(["};", ""])
    return "\n".join(lines)


def write_if_changed(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_text(encoding="utf-8") == content:
        return
    path.write_text(content, encoding="utf-8")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--calibration-dir", type=Path, required=True)
    parser.add_argument("--output-header", type=Path, required=True)
    parser.add_argument("--output-source", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        calibrations = discover_calibrations(args.calibration_dir)
    except CalibrationError as error:
        print(f"Calibration generation failed: {error}")
        return 1

    write_if_changed(args.output_header, render_header())
    write_if_changed(args.output_source, render_source(calibrations))
    embedded = ", ".join(str(servo_id) for servo_id in calibrations) or "none"
    print(f"Embedded linear calibration for servo(s): {embedded}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
