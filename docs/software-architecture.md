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

### app / main

main supplies board-specific audio and servo configuration, initializes the
servos at 0 degrees, and registers all console commands. It formats the compact
stream records without coupling audio processing to servo commands.

## Configuration

Hardware-dependent servo values are centralized in SERVO_CONFIG in
main/main.c; microphone pins and target bands are centralized in AUDIO_CONFIG.
Current configuration includes:

- I2S BCLK GPIO10, WS/LRCLK GPIO11, and data GPIO12.
- Target frequencies 73, 145, 213, and 395 Hz with ±20 Hz bands.
- Servo signal GPIOs 36, 37, 38, and 39.
- Per-servo safe pulse-width limits, center offset, permitted angles, and
  startup angle.

Audio processing never commands a servo. A future coordination layer may use
the measurements and a separately defined transfer function, but neither
automatic servo response nor that transfer function exists yet.
