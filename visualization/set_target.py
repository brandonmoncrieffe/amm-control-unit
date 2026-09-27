"""Retarget a cavity's blocked frequency over WiFi.

This only changes which frequency the microphone measures for that band
(and, if you're running visualization/autotune.py, which frequency it
steers that cavity's piston toward). It does not move a servo directly.

Usage:
    python3 -m visualization.set_target --cavity 2 --hz 250
"""

from __future__ import annotations

import argparse
import sys

from visualization.esp32_client import DEFAULT_BASE_URL, Esp32Client


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--esp32-host", default=DEFAULT_BASE_URL)
    parser.add_argument("--cavity", type=int, required=True, choices=[1, 2, 3, 4])
    parser.add_argument("--hz", type=float, required=True)
    args = parser.parse_args()

    client = Esp32Client(args.esp32_host)
    applied_hz = client.set_target_frequency(args.cavity, args.hz)
    if applied_hz is None:
        print(
            f"Failed to retarget cavity {args.cavity} — check the ESP32 is "
            "reachable and the frequency (+/- its bandwidth) fits 0..Nyquist.",
            file=sys.stderr,
        )
        return 1

    print(f"Cavity {args.cavity} now targets {applied_hz:.3f} Hz")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
