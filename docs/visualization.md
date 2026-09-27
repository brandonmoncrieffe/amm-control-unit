# Live Web Dashboard

## What it shows

A browser dashboard with:

- 4 cavity panels — one per servo, showing its commanded piston depth (or raw
  angle if that servo has no calibration data yet), the frequency band it is
  currently set to block, and the live dBFS level measured in that band.
- A live spectrum chart (0–1000 Hz, where all four configured target bands
  live) with a dashed marker at each cavity's target frequency, so you can see
  whether a resonator's target lines up with an actual peak in the incoming
  noise.

This is **read-only monitoring**. It never sends servo commands to the ESP32.
Real-time servo adjustment from the dashboard is a later addition.

## Data source

The ESP32 hosts its own WiFi access point (`wifi_status_server` component,
SSID/password configured via `idf.py menuconfig` under "AMM Control Unit")
and serves two read-only JSON endpoints from `esp_http_server`, independent
of the USB Serial/JTAG console:

- `GET /state` — servo commanded angles plus the latest target-band and RMS
  audio measurement.
- `GET /spectrum` — the latest complete FFT in relative dBFS.

Both can run at the same time as the existing serial console, `stream on`,
and `tools/live_spectrum.py` — they use entirely separate transports.

## Running it

Join the ESP32's access point first (default SSID `amm-control-unit`, see
`idf.py menuconfig` for the configured password), then:

```sh
python3 -m venv .venv
source .venv/bin/activate
python3 -m pip install -r visualization/requirements.txt
python3 -m visualization.server
```

Open `http://127.0.0.1:8000` in a browser. If the ESP32's AP gateway isn't at
the default `192.168.4.1`, pass `--esp32-host http://<ip>`.

## Depth vs. angle

No `calibration/servo_N.csv` files exist in this repository yet. Until you
run `tools/servo_calibration.py` for a given servo, its cavity panel shows
the raw commanded angle (0–180°) instead of a depth in millimetres, labeled
accordingly. Once a calibration CSV exists, restart the dashboard; it builds a
piecewise-linear angle→depth interpolation from each `calibration/servo_N.csv`
at startup.
