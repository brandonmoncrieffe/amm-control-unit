# AMM Control Unit

ESP-IDF firmware for independently controlling four SG90 positional servos
from an interactive serial console. Audio input and automatic servo behavior
are not implemented.

## Current target

- ESP-IDF target: `esp32s3`
- ESP-IDF version used for initial verification: 6.0.1
- Exact development board: to be confirmed

The connected device reported 8 MB of flash and 2 MB of embedded PSRAM during
inspection. These observations are not yet used to configure the firmware.

## Build

Activate your ESP-IDF environment using the VS Code ESP-IDF extension or the
export script from your ESP-IDF installation, then build from the repository
root:

```sh
idf.py build
```

The generated `sdkconfig` and `build/` directory are local artifacts and are
not committed.

## Servo wiring

| Servo | Signal GPIO |
| --- | --- |
| 1 | GPIO36 |
| 2 | GPIO37 |
| 3 | GPIO38 |
| 4 | GPIO39 |

Power the servos from the external regulated 5 V supply, not from the ESP32.
Connect the external supply ground to ESP32 GND before connecting the signal
wires.

## Flash and monitor

After selecting the correct serial port, the standard ESP-IDF workflow is:

```sh
idf.py -p PORT flash monitor
```

Exit the monitor with `Ctrl-]`. Flashing is intentionally not part of the
initial repository verification.

The console prompt is `servo>`. Available commands are:

```text
set <servo 1-4> <angle>
all <angle>
center
status
help
```

Examples:

```text
set 1 0
set 1 90
set 4 180
all 90
```

Requested angles are clamped first to 0–180 degrees and then to each servo's
configured permitted range.

## Servo calibration

Per-servo calibration is defined in `SERVO_CONFIG` in `main/main.c`. The
initial defaults are 500 microseconds at 0 degrees, 1500 microseconds at 90
degrees, and 2500 microseconds at 180 degrees, with a startup angle of 0
degrees. These are starting values only. Calibrate minimum/maximum pulse width,
center offset, and permitted angles for each physical servo and mechanism
before exercising the full range.

For mechanism calibration, install the host tool dependency and run the
interactive calibration assistant after flashing the firmware:

```sh
python3 -m venv .venv
source .venv/bin/activate
python3 -m pip install -r tools/requirements.txt
python3 tools/servo_calibration.py --servo 1 --start-angle 0 --end-angle 30 --step 3
```

Close `idf.py monitor` before starting the assistant because only one program
can own the serial port. The assistant requires a typed safety confirmation,
commands one angle at a time, asks you to enter the measured mechanism travel
in millimetres, and writes a CSV file under `calibration/`. Start with a small,
known-safe angle range rather than assuming the complete 0–180 degree range is
mechanically safe.

Hardware decisions and the planned software modules are recorded in
[`docs/hardware.md`](docs/hardware.md) and
[`docs/software-architecture.md`](docs/software-architecture.md). A walkthrough
of the implemented servo path is in
[`docs/servo-control.md`](docs/servo-control.md).
