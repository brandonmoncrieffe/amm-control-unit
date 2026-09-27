# Servo Control Walkthrough

## Command path

The current path for a command such as `set 1 30` is:

```text
serial console
  -> set_command() in main/main.c
  -> servo_set_angle() in components/servo_control/servo_control.c
  -> angle and per-servo safety clamping
  -> angle-to-pulse conversion
  -> LEDC duty update
  -> PWM signal on GPIO36
```

`main/main.c` owns application-level details: the four GPIO assignments, the
serial commands, and the `servo>` prompt. The reusable `servo_control`
component owns PWM setup and servo calculations. Its public interface and
configuration types are declared in
`components/servo_control/include/servo_control.h`.

## Startup and channels

`app_main()` initializes servo control, registers commands, and starts the
console. `servo_init()` configures one shared LEDC timer at 50 Hz and four LEDC
channels. The channels use GPIO36, GPIO37, GPIO38, and GPIO39. Each servo is
initially commanded to its configured `initial_angle_deg`, currently 0
degrees. There is no continuous motion task or automatic behaviour.

All four channels share the frequency and timer but have independent duty
values, so each servo can hold a different commanded angle.

## Angle conversion

The default calibration values are 500 microseconds at 0 degrees and 2500
microseconds at 180 degrees. The nominal centre is their midpoint (1500
microseconds), adjusted by `center_offset_us`.

The conversion is piecewise linear:

- 0–90 degrees interpolates from the minimum pulse to the adjusted centre.
- 90–180 degrees interpolates from the adjusted centre to the maximum pulse.

The pulse is converted to LEDC duty counts for a 20,000-microsecond period
(50 Hz) and 14-bit resolution. `ledc_set_duty()` stages the new value and
`ledc_update_duty()` applies it.

`servo_set_angle()` first clamps every request to the public 0–180 degree
range, then to that servo's configured `min_angle_deg` and `max_angle_deg`.
The log shows the requested angle, clamped angle, and resulting pulse width.
`status` reports the last commanded angle; it is not physical position
feedback.

## Per-servo calibration

Each entry in `SERVO_CONFIG` can independently specify:

- signal GPIO;
- minimum and maximum pulse width;
- centre pulse offset;
- minimum and maximum permitted angle; and
- startup angle.

The current `SERVO_CALIBRATION_DEFAULT(...)` entries use identical defaults.
Those pulse limits and the complete 0–180 degree range are not guaranteed safe
for every SG90 or linkage. Determine conservative limits experimentally and
then replace each default entry with explicit values.

## Linear actuation layer

The servo component intentionally works in shaft-angle commands. A future
mechanism or actuator layer should convert a requested distance in millimetres
to an angle for the specific servo/linkage, then call `servo_set_angle()`:

```text
requested distance (mm)
  -> per-mechanism calibration curve/interpolation
  -> safe angle (degrees)
  -> servo_control
  -> PWM
```

The Python calibration assistant records the angle/distance points needed to
build that curve. Measuring both directions is recommended because gears and
linkages commonly have backlash and hysteresis. A lookup table with piecewise
linear interpolation is usually a better first implementation than forcing a
single polynomial through sparse measurements.

No linear-distance API, position sensor feedback, I2S microphone processing,
or automatic servo behaviour is implemented yet.
