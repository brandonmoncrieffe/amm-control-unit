# Hardware

This document records confirmed hardware facts and current wiring.

## Controller

| Item | Value |
| --- | --- |
| ESP32 board | TBD: exact vendor and board model |
| ESP32 chip | ESP32-S3, revision 0.2 (detected) |
| Flash | 8 MB detected; configuration TBD after board confirmation |
| PSRAM | 2 MB embedded PSRAM detected; configuration TBD after board confirmation |

The detected memory sizes are observations only. They are not currently
enabled or otherwise configured by this project.

## I2S microphone

| Item | Value |
| --- | --- |
| Microphone model | Adafruit ICS-43434 I2S MEMS microphone breakout |
| VDD | ESP32 3.3 V |
| Ground | ESP32 GND |
| BCLK / SCK | GPIO10 |
| WS / LRCLK | GPIO11 |
| DOUT / DATA | GPIO12 |
| L/R select | GND, selecting the left I2S slot |

The ICS-43434 is a 3.3 V device and must not be connected to 5 V. GPIO10,
GPIO11, and GPIO12 are exposed on the selected ESP32-S3 DevKitC-1 and do not
conflict with servo GPIO36-39. No microphone level shifter is required because
the microphone and ESP32 use 3.3 V logic.

## Servos

| Item | Value |
| --- | --- |
| Servo type | Four SG90 9 g positional micro servos |
| Servo 1 signal GPIO | GPIO36 |
| Servo 2 signal GPIO | GPIO37 |
| Servo 3 signal GPIO | GPIO38 |
| Servo 4 signal GPIO | GPIO39 |
| External servo supply | Regulated 5 V; current rating must cover combined startup and stall current |

The servo power supply must be sized for the combined startup and stall-current
demands of the installed servos. Do not assume the ESP32 board's regulator or
USB supply can power the servos.

GPIO19 was deliberately not used because it is the ESP32-S3 native USB D− pin.
Keeping GPIO19 and GPIO20 free preserves the USB Serial/JTAG console used to
command the servos.

The detected chip has 2 MB of in-package Quad SPI PSRAM. GPIO36 and GPIO37
must not be used for servos if the hardware is later changed to a variant with
Octal flash or Octal PSRAM, because those pins are part of that memory bus.

## Grounding and logic levels

- Common ground wiring: required between ESP32 GND and the external 5 V servo
  supply ground. The microphone also uses ESP32 GND.
- Level shifting: none is currently specified for the 3.3 V ESP32 servo signal;
  verify the selected servos accept this input level before final assembly.
- Power distribution, decoupling, protection, and connector details: TBD.

Review power wiring, supply capacity, and servo calibration before commanding
the full travel range. Firmware startup commands servo 2 to 0 degrees and the
other three servos to 180 degrees;
verify the calibrated minimum pulse does not drive any servo into its mechanical
stop.
