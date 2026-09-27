# Software Architecture

The firmware provides manual control of four servos and continuous acoustic
analysis through an ESP-IDF serial console. Automatic application behavior
remains future work.

## Components

### audio_processing (implemented)

Owns ICS-43434 I2S acquisition, signed 24-bit sample conversion, DC removal,
RMS calculation, Hann windowing, the 2048-point FFT, target-band integration,
and a thread-safe latest-result snapshot.

The component uses the ESP-IDF 6 channel-based standard-I2S receive API at
16 kHz with 32-bit left-channel slots. It invokes a short application callback
after each non-overlapping block and otherwise has no dependency on the serial
console or servo component.

### servo_control (implemented)

The reusable servo_control component owns a shared 50 Hz LEDC timer, four
independent LEDC channels, per-servo calibration, angle clamping, and the last
commanded angle for each servo.

Its public API provides initialization, individual angle commands, all-servo
commands, centering, and commanded-angle queries. Each calibration entry
contains GPIO, minimum and maximum pulse width, center offset, permitted angle
range, and initial angle. The default 500/1500/2500 microsecond calibration
must be adjusted for the physical servos and mechanism.

### linear_actuator (implemented)

Owns the optional per-servo millimetre-to-angle mapping. Its build generator
discovers available mechanism CSVs, validates their outward sweeps, and emits
piecewise-linear tables into the build directory. A missing CSV leaves that
servo uncalibrated without affecting its angle controls; an invalid existing
CSV fails configuration.

Length commands clamp to the measured range, interpolate an angle, and call
the unchanged servo_control API. The layer moves directly and provides only
open-loop length estimates.

### wifi_status_server (implemented)

Owns a WiFi access point (`esp_wifi` AP mode) and an `esp_http_server`
exposing three endpoints, independent of the USB Serial/JTAG console so both
can run at once:

- `GET /state` and `GET /spectrum` (read-only) for the host-side live
  dashboard (see [`docs/visualization.md`](visualization.md)). These only
  call `servo_control`'s and `audio_processing`'s existing public getters.
- `POST /servo?id=<1-4>&angle_deg=<int>` (mutating) forwards straight to
  `servo_set_angle()`, so it applies exactly the same clamping as the serial
  console's `set` command, and responds with the angle actually applied
  after clamping. This is what [`docs/auto-tuning.md`](auto-tuning.md)'s
  background control loop uses to move servos over WiFi; nothing else on the
  ESP32 calls it automatically.

SSID/password/channel are `Kconfig` options (`main/Kconfig.projbuild`), kept
out of tracked source in the gitignored `sdkconfig`.

### app / main

main supplies board-specific audio and servo configuration, initializes the
servos at their per-servo startup angles, starts the WiFi status server, and
registers the angle, length, demo, and audio console commands. It formats the
compact stream records without coupling audio processing to servo commands.

## Configuration

Hardware-dependent servo values are centralized in SERVO_CONFIG in
main/main.c; microphone pins and target bands are centralized in AUDIO_CONFIG.
Current configuration includes:

- I2S BCLK GPIO10, WS/LRCLK GPIO11, and data GPIO12.
- Target frequencies 73, 145, 213, and 395 Hz with ±20 Hz bands.
- Servo signal GPIOs 36, 37, 38, and 39.
- Per-servo safe pulse-width limits, center offset, permitted angles, and
  startup angle.
- Two-stage demo measurement: stage 1 records an original-position baseline;
  stage 2 applies 23 mm to servo 1 and 6.5 mm to servo 2 and reports the first
  two target-band reductions. Servos 3 and 4 remain unchanged.

Audio processing never commands a servo. A future coordination layer may use
the measurements and a separately defined transfer function, but neither
automatic servo response nor that transfer function exists yet.

The WiFi status dashboard access point's SSID, password, channel, and maximum
client count are `Kconfig` options under "AMM Control Unit" (`idf.py
menuconfig`), defaulting to SSID `amm-control-unit`. The default AP gateway
address is `192.168.4.1`.
