# AMM Control Unit

ESP-IDF firmware for independently controlling four SG90 positional servos
and measuring four acoustic target bands with an ICS-43434 I2S microphone.
Servo control remains manual; automatic acoustic-to-servo behavior is not
implemented.

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

## I2S microphone wiring

The confirmed microphone is the Adafruit ICS-43434 I2S MEMS microphone
breakout.

| Microphone pin | ESP32-S3 connection |
| --- | --- |
| VDD / 3V | 3.3 V |
| GND | ESP32 GND |
| BCLK / SCK | GPIO10 |
| WS / LRCLK | GPIO11 |
| DOUT / DATA | GPIO12 |
| L/R / SEL | GND (left channel) |

Do not power the microphone from 5 V. GPIO10, GPIO11, and GPIO12 are exposed
on the selected ESP32-S3 DevKitC-1 and do not conflict with servo GPIO36-39.

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
length <servo 1-4> <millimetres>
lengthstatus
demo <1|2>
stream <on|off>
level
spectrum
help
```

Examples:

```text
set 1 0
set 1 90
set 4 180
all 90
length 1 12
lengthstatus
demo 1
demo 2
```

Requested angles are clamped first to 0–180 degrees and then to each servo's
configured permitted range.

Length commands are available only for servos with a valid
`calibration/servo_N.csv` at build time. Requested lengths are converted to
angles with piecewise-linear interpolation and clamped to the measured range.
The original angle commands remain available independently.

`demo 1` returns servos 1 and 2 to their original 0 mm positions, waits 1.5
seconds, and records an eight-block baseline for the 73 Hz and 145 Hz target
bands. `demo 2` requires that in-memory baseline, moves servo 1 to 23 mm and
servo 2 to 6.5 mm, repeats the measurement, and reports
`baseline dBFS - demo dBFS`; a positive result means suppression. Each average
is calculated in linear power before conversion back to relative dBFS. Servos
3 and 4 are left unchanged.

Audio acquisition and FFT processing run continuously. The stream-on command
enables compact target-band records at approximately 7.8 updates per second;
stream-off stops only the serial records, not microphone processing. The
level command prints one target/RMS record and spectrum prints the latest
complete FFT. Reported audio levels are relative dBFS, not calibrated dB SPL.

## Live acoustic plot

Install the host dependencies:

~~~sh
python3 -m venv .venv
source .venv/bin/activate
python3 -m pip install -r tools/requirements.txt
~~~

Close the ESP-IDF monitor, then start the plot:

~~~sh
python3 tools/live_spectrum.py --port /dev/cu.usbmodem1101
~~~

The plotter automatically enables streaming. Its terminal accepts
baseline start, baseline stop, servo commands such as set 1 30, and quit.
Opening the serial port may reset the board and command the servos to their
configured startup positions: servo 2 to 0 degrees and the others to 180
degrees.

## Live web dashboard

The ESP32 also hosts its own WiFi access point (default SSID
`amm-control-unit`, password set via `idf.py menuconfig` under "AMM Control
Unit") and a read-only HTTP status API (`GET /state`, `GET /spectrum`),
independent of the USB serial console — both can run at the same time. A
Python dashboard polls that API and renders the 4 cavities plus a live
spectrum chart in a browser:

```sh
python3 -m venv .venv
source .venv/bin/activate
python3 -m pip install -r visualization/requirements.txt
python3 -m visualization.server
```

Join the ESP32's access point first, then open `http://127.0.0.1:8000`. See
[`docs/visualization.md`](docs/visualization.md) for details, including how
depth is shown before a servo has been calibrated. This dashboard is
read-only; it does not send servo commands.

## Servo calibration

Per-servo calibration is defined in `SERVO_CONFIG` in `main/main.c`. The
initial defaults are 500 microseconds at 0 degrees, 1500 microseconds at 90
degrees, and 2500 microseconds at 180 degrees. Servo 2 starts at 0 degrees and
is limited to 160 degrees because its mechanism plateaus there; the other
servos start at 180 degrees. These are starting values only. Calibrate
minimum/maximum pulse width,
center offset, and permitted angles for each physical servo and mechanism
before exercising the full range.

For mechanism calibration, install the host tool dependency and run the
interactive calibration assistant after flashing the firmware:

```sh
python3 -m venv .venv
source .venv/bin/activate
python3 -m pip install -r tools/requirements.txt
python3 tools/servo_calibration.py --servo 1 --start-angle 180 --end-angle 0 --step 40
```

Close `idf.py monitor` before starting the assistant because only one program
can own the serial port. The assistant requires a typed safety confirmation,
commands one angle at a time, asks you to enter the measured mechanism travel
in millimetres, and writes a CSV file under `calibration/`. Start with a small,
known-safe angle range rather than assuming the complete 0–180 degree range is
mechanically safe. After reviewing a CSV, rebuild and flash the firmware to
embed it. Missing servo CSVs are allowed; only those servos remain unavailable
to the `length` command.

Hardware decisions and the planned software modules are recorded in
[`docs/hardware.md`](docs/hardware.md) and
[`docs/software-architecture.md`](docs/software-architecture.md). Walkthroughs
are available for
[`servo control`](docs/servo-control.md), the
[`live acoustic demo`](docs/acoustic-demo.md), and the
[`live web dashboard`](docs/visualization.md).
