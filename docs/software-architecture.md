# Software Architecture

The firmware currently provides manual control of four servos through an
ESP-IDF serial console. Audio acquisition and automatic application behavior
remain future work.

## Future modules

### `audio_input`

Own I2S microphone configuration and sample acquisition. Its eventual public
interface should isolate ESP-IDF I2S details from the coordinating application
and report initialization or acquisition failures explicitly.

The microphone model, sample format, sample rate, channel selection, buffering,
and GPIO assignments remain TBD.

### `servo_control` (implemented)

The reusable `servo_control` component owns a shared 50 Hz LEDC timer, four
independent LEDC channels, per-servo calibration, angle clamping, and the last
commanded angle for each servo.

Its public API provides initialization, individual angle commands, all-servo
commands, centering, and commanded-angle queries. Each calibration entry
contains GPIO, minimum and maximum pulse width, center offset, permitted angle
range, and initial angle. The default 500/1500/2500 microsecond calibration
must be adjusted for the physical servos and mechanism.

### `app` / `main`

`main` supplies the board-specific GPIO and calibration configuration,
initializes the servos at 0 degrees, and registers the `set`, `all`, `center`,
`status`, and `help` console commands. It will later coordinate audio input,
servo control, error handling, and product behavior.

## Configuration

Hardware-dependent servo values are centralized in `SERVO_CONFIG` in
`main/main.c`. Current configuration includes:

- I2S BCLK, WS/LRCLK, and data GPIOs.
- Servo signal GPIOs 36, 37, 38, and 39.
- Per-servo safe pulse-width limits, center offset, permitted angles, and
  startup angle.
- Any board-specific peripheral or resource selections.

I2S microphone pins and audio configuration remain unassigned. They must not
conflict with the servo outputs or the native USB pins when added later.
