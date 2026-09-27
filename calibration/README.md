# Mechanism Calibration Data

This directory is for measurements produced by `tools/servo_calibration.py`.
The CSV files relate a commanded servo angle to manually measured linear
travel. They are reviewed source data and become build-time inputs to the
firmware's `linear_actuator` component.

Measure each assembled servo, horn, gear, and linkage independently. A useful
first pass is a small step such as 3 degrees over a mechanically verified safe
range. Use `--bidirectional` to measure both directions; differences between
the outward and return measurements expose backlash, compliance, and servo
deadband.

The assistant leaves the servo at the final commanded angle. It never assumes
that 0 or 180 degrees is safe, and it does not automatically rewrite C source
files. Opening a serial connection may reset the ESP32; with the current
firmware, reset commands servo 2 to 0 degrees and the other servos to their
configured 180-degree startup positions.

The firmware build discovers `servo_1.csv` through `servo_4.csv`. It embeds
only non-empty outward measurements, requires at least two unique points, and
requires distance to be monotonic with angle. A repeated distance is accepted
only at a terminal plateau and is reduced to the angle nearest the varying
region; interior plateaus remain invalid. Return measurements are kept in the
CSV for backlash analysis but are not embedded. Missing files are allowed;
malformed or ambiguous existing files fail the build.

After creating or changing a CSV, rebuild and flash:

```sh
idf.py build flash
```

The CSV files are not read from the ESP32 filesystem at runtime.
