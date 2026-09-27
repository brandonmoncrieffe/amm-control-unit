# Live Acoustic Demonstration

## Measurement pipeline

The ICS-43434 supplies signed 24-bit samples in 32-bit I2S slots. The receiver
selects the left slot because L/R is grounded. Each raw signed 32-bit word is
arithmetically shifted right by eight bits and normalized against 2^23.

Audio is sampled at 16 kHz in non-overlapping blocks of 2048 samples, giving a
128 ms block duration and 7.8125 Hz FFT-bin spacing. For each block, the
firmware:

1. removes the mean;
2. calculates unwindowed time-domain RMS;
3. applies a Hann window;
4. calculates a 2048-point FFT with ESP-DSP;
5. normalizes the 1025 single-sided bins for Hann-window energy; and
6. sums linear bin power inside each configured target band before converting
   the result to dBFS.

The initial targets are 73, 145, 213, and 395 Hz, each with a ±20 Hz band.
Logarithmic results have a -120 dBFS floor. These are relative digital levels,
not calibrated sound-pressure levels and not dB SPL.

## Console commands

~~~text
stream on
stream off
level
spectrum
~~~

Acquisition and FFT processing run continuously. Streaming is disabled at
startup. Enabling it prints the configuration once, then one compact
measurement for each completed block:

~~~text
AUDIO_CONFIG,16000,2048,20.000,73.000,145.000,213.000,395.000
SPECTRUM,<timestamp_ms>,<73_dbfs>,<145_dbfs>,<213_dbfs>,<395_dbfs>,<rms_dbfs>
~~~

The level command prints one SPECTRUM record from the latest block. The
spectrum command prints the complete latest spectrum in chunks:

~~~text
FFT_BEGIN,<timestamp_ms>,16000,2048,7.812500,1025
FFT,<start_bin>,<up-to-32-dbfs-values>
FFT_END,<timestamp_ms>
~~~

The set, all, and center commands continue to control only the servos. They
neither start nor stop audio acquisition.

## Host plot and baseline

Close the ESP-IDF monitor because only one process can own the serial port,
then run:

~~~sh
python3 tools/live_spectrum.py --port /dev/cu.usbmodem1101 --baud 115200
~~~

The upper graph shows a rolling absolute dBFS history. The lower graph shows
change relative to a frozen baseline. Enter baseline start in the plotter's
terminal, collect a stable interval, then enter baseline stop. Baseline samples
are averaged in linear power before conversion back to dB. Negative delta
values indicate reduced target-band energy relative to that baseline.

Servo commands can be entered in the same terminal while the plot is open.
The set, all, and center commands are forwarded to the ESP32 and annotated on
the plot. Opening the serial port can reset the ESP32 and move every servo to
its configured startup position (servo 2 at 0 degrees and the others at 180
degrees).

## Experimental controls

Meaningful before/after suppression measurements require:

- a fixed microphone position and orientation;
- a fixed sound-source position;
- stable source amplitude;
- the same recorded servo configuration whenever a state is repeated;
- adequate settling time after servo movement; and
- identical room and mounting conditions where practical.

Keep the microphone port unobstructed. Servo electrical and mechanical noise
can contaminate a measurement, so compare settled states rather than treating
movement transients as acoustic suppression.

No microphone-to-SPL calibration, transfer function, automatic frequency
response, or automatic servo control is implemented.

## Two-position demonstration

`demo 1` commands calibrated servos 1 and 2 to 0 mm, waits 1.5 seconds for
settling, and captures eight fresh FFT-block measurements. It averages the 73
Hz and 145 Hz target bands in linear power and stores the result in RAM as the
baseline.

`demo 2` requires that baseline, commands servo 1 to 23 mm and servo 2 to 6.5
mm, repeats the same measurement, and prints baseline, demo, and drop values.
Drop is `baseline dBFS - demo dBFS`, so positive values indicate measured
suppression. All values remain relative dBFS, not calibrated dB SPL. Resetting
the board clears the stored baseline, so `demo 1` must be run again.
