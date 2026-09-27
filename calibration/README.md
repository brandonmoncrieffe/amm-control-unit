# Mechanism Calibration Data

This directory is for measurements produced by `tools/servo_calibration.py`.
The CSV files relate a commanded servo angle to manually measured linear
travel. They are source data, not firmware configuration, and should be
reviewed before any mapping is added to the firmware.

Measure each assembled servo, horn, gear, and linkage independently. A useful
first pass is a small step such as 3 degrees over a mechanically verified safe
range. Use `--bidirectional` to measure both directions; differences between
the outward and return measurements expose backlash, compliance, and servo
deadband.

The assistant leaves the servo at the final commanded angle. It never assumes
that 0 or 180 degrees is safe, and it does not automatically rewrite C source
files. Opening a serial connection may reset the ESP32; with the current
firmware, reset commands all four servos to their configured 0-degree startup
position.
