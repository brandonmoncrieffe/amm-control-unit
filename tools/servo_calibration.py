#!/usr/bin/env python3
"""Interactively record servo angle versus measured linear travel."""

from __future__ import annotations

import argparse
import csv
import re
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterator


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
ESPRESSIF_USB_VENDOR_ID = 0x303A
SERVO_GPIOS = {1: 36, 2: 37, 3: 38, 4: 39}
SERIAL_PROMPT = b"servo>"
CLAMPED_ANGLE_PATTERN = re.compile(r"clamped=(-?\d+) deg")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Command one servo through a chosen angle range and record manually "
            "measured linear travel as CSV."
        )
    )
    parser.add_argument("--servo", type=int, choices=range(1, 5), required=True)
    parser.add_argument("--start-angle", type=int, required=True)
    parser.add_argument("--end-angle", type=int, required=True)
    parser.add_argument(
        "--step", type=int, default=3, help="positive step size in degrees (default: 3)"
    )
    parser.add_argument(
        "--port",
        help="serial port; omitted to auto-detect one connected Espressif device",
    )
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument(
        "--settle-seconds",
        type=float,
        default=1.0,
        help="delay before asking for a measurement (default: 1.0)",
    )
    parser.add_argument(
        "--bidirectional",
        action="store_true",
        help="also measure the range in reverse to reveal backlash/hysteresis",
    )
    parser.add_argument(
        "--output",
        type=Path,
        help="CSV path (default: calibration/servo_N.csv)",
    )
    parser.add_argument(
        "--overwrite",
        action="store_true",
        help="replace an existing output file",
    )
    args = parser.parse_args()

    if not 0 <= args.start_angle <= 180 or not 0 <= args.end_angle <= 180:
        parser.error("start and end angles must both be within 0..180 degrees")
    if args.start_angle == args.end_angle:
        parser.error("start and end angles must be different")
    if args.step <= 0:
        parser.error("--step must be greater than zero")
    if args.settle_seconds < 0:
        parser.error("--settle-seconds cannot be negative")
    if args.output is None:
        args.output = REPOSITORY_ROOT / "calibration" / f"servo_{args.servo}.csv"
    elif not args.output.is_absolute():
        args.output = Path.cwd() / args.output

    return args


def import_serial_modules():
    try:
        import serial
        from serial.tools import list_ports
    except ImportError:
        print(
            "pyserial is required. Install it with:\n"
            "  python3 -m pip install -r tools/requirements.txt",
            file=sys.stderr,
        )
        raise SystemExit(2) from None
    return serial, list_ports


def select_port(requested_port: str | None, list_ports) -> str:
    if requested_port:
        return requested_port

    ports = list(list_ports.comports())
    espressif_ports = [port for port in ports if port.vid == ESPRESSIF_USB_VENDOR_ID]
    if len(espressif_ports) == 1:
        return espressif_ports[0].device

    detected = "\n".join(
        f"  {port.device}: {port.description}" for port in ports
    ) or "  (none)"
    reason = "none were" if not espressif_ports else "more than one was"
    raise RuntimeError(
        f"Could not auto-select a serial port because {reason} detected.\n"
        f"Detected ports:\n{detected}\n"
        "Close idf.py monitor, then specify the board explicitly with --port."
    )


def inclusive_angles(start: int, end: int, step: int) -> list[int]:
    direction = 1 if end > start else -1
    signed_step = direction * step
    angles = list(range(start, end, signed_step))
    if not angles or angles[-1] != end:
        angles.append(end)
    return angles


def measurement_sequence(
    start: int, end: int, step: int, bidirectional: bool
) -> Iterator[tuple[str, int]]:
    outward = inclusive_angles(start, end, step)
    yield from (("outward", angle) for angle in outward)
    if bidirectional:
        yield from (("return", angle) for angle in reversed(outward))


def read_until_prompt(connection, timeout_seconds: float = 8.0) -> str:
    deadline = time.monotonic() + timeout_seconds
    response = bytearray()
    while time.monotonic() < deadline:
        chunk = connection.read(connection.in_waiting or 1)
        if chunk:
            response.extend(chunk)
            if SERIAL_PROMPT in response:
                return response.decode(errors="replace")
    raise TimeoutError("Timed out waiting for the firmware's 'servo>' prompt")


def send_command(connection, command: str) -> str:
    connection.reset_input_buffer()
    connection.write((command + "\r").encode())
    connection.flush()
    response = read_until_prompt(connection)
    lowered = response.lower()
    if "failed" in lowered or "error" in lowered:
        raise RuntimeError(f"Firmware rejected '{command}':\n{response.strip()}")
    return response


def commanded_angle_from_response(response: str) -> int:
    match = CLAMPED_ANGLE_PATTERN.search(response)
    if match is None:
        raise RuntimeError(
            "The firmware response did not contain its clamped angle. "
            "Confirm that INFO logging is enabled and compatible firmware is flashed."
        )
    return int(match.group(1))


def request_measurement(servo: int, angle: int) -> str | None:
    while True:
        entered = input(
            f"Servo {servo} at {angle} degrees — measured travel in mm "
            "(s=skip, q=save and quit): "
        ).strip()
        if entered.lower() == "q":
            return None
        if entered.lower() == "s":
            return ""
        try:
            measurement = float(entered)
        except ValueError:
            print("Enter a number, 's', or 'q'.")
            continue
        return format(measurement, ".10g")


def print_safety_summary(args: argparse.Namespace, port: str) -> None:
    print(
        "\nCalibration plan\n"
        f"  Servo: GPIO {SERVO_GPIOS[args.servo]} (servo {args.servo})\n"
        f"  Range: {args.start_angle} to {args.end_angle} degrees, "
        f"{args.step}-degree steps\n"
        f"  Return sweep: {'yes' if args.bidirectional else 'no'}\n"
        f"  Port: {port}\n"
        f"  Output: {args.output}\n\n"
        "Before continuing:\n"
        "  - Close the ESP-IDF serial monitor.\n"
        "  - Use an external regulated 5 V servo supply and common ground.\n"
        "  - Verify this angle range cannot jam the linkage or hit a hard stop.\n"
        "  - Be ready to disconnect servo power if the mechanism binds.\n"
        "  - Connecting may reset the board; servo 2 starts at 0 degrees and "
        "the others at 180 degrees.\n"
    )


def main() -> int:
    args = parse_args()
    serial, list_ports = import_serial_modules()

    try:
        port = select_port(args.port, list_ports)
    except RuntimeError as error:
        print(error, file=sys.stderr)
        return 2

    print_safety_summary(args, port)
    if input("Type START to begin moving the servo: ").strip() != "START":
        print("Calibration cancelled; no servo command was sent.")
        return 0

    args.output.parent.mkdir(parents=True, exist_ok=True)
    mode = "w" if args.overwrite else "x"
    try:
        output_file = args.output.open(mode, newline="", encoding="utf-8")
    except FileExistsError:
        print(
            f"Output already exists: {args.output}\n"
            "Choose another --output or pass --overwrite.",
            file=sys.stderr,
        )
        return 2

    fieldnames = [
        "timestamp_utc",
        "servo_id",
        "gpio",
        "sweep_direction",
        "commanded_angle_deg",
        "measured_distance_mm",
    ]
    completed = 0
    try:
        writer = csv.DictWriter(output_file, fieldnames=fieldnames)
        writer.writeheader()
        output_file.flush()

        with serial.Serial(
            port=port,
            baudrate=args.baud,
            timeout=0.1,
            write_timeout=1.0,
        ) as connection:
            connection.dtr = False
            connection.rts = False
            time.sleep(1.0)
            connection.reset_input_buffer()
            connection.write(b"\r")
            connection.flush()
            read_until_prompt(connection)

            for direction, angle in measurement_sequence(
                args.start_angle, args.end_angle, args.step, args.bidirectional
            ):
                response = send_command(connection, f"set {args.servo} {angle}")
                commanded_angle = commanded_angle_from_response(response)
                if commanded_angle != angle:
                    raise RuntimeError(
                        f"Requested {angle} degrees, but the firmware clamped it to "
                        f"{commanded_angle} degrees. Narrow the calibration range to "
                        "the firmware's configured safe limits."
                    )
                time.sleep(args.settle_seconds)
                measured_mm = request_measurement(args.servo, angle)
                if measured_mm is None:
                    break
                writer.writerow(
                    {
                        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
                        "servo_id": args.servo,
                        "gpio": SERVO_GPIOS[args.servo],
                        "sweep_direction": direction,
                        "commanded_angle_deg": commanded_angle,
                        "measured_distance_mm": measured_mm,
                    }
                )
                output_file.flush()
                completed += 1
    except KeyboardInterrupt:
        print("\nInterrupted; measurements already entered have been saved.")
    except (OSError, RuntimeError, TimeoutError, serial.SerialException) as error:
        print(f"Calibration stopped: {error}", file=sys.stderr)
        return 1
    finally:
        output_file.close()

    print(f"Saved {completed} measurement(s) to {args.output}")
    print("The servo was left at its last commanded angle.")
    print("Rebuild and flash the firmware to embed updated calibration data.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
