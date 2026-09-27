#!/usr/bin/env python3
"""Plot live target-frequency measurements from the AMM control unit."""

from __future__ import annotations

import argparse
import math
import queue
import sys
import threading
import time
from collections import deque
from dataclasses import dataclass
from typing import Sequence


ESPRESSIF_USB_VENDOR_ID = 0x303A
SERIAL_PROMPT = b"servo>"
TARGET_COUNT = 4
FORWARDED_COMMANDS = {
    "set",
    "all",
    "center",
    "status",
    "level",
    "spectrum",
    "stream",
    "help",
}
ACTUATION_COMMANDS = {"set", "all", "center"}


@dataclass(frozen=True)
class AudioConfig:
    sample_rate_hz: int
    fft_size: int
    half_bandwidth_hz: float
    target_frequencies_hz: tuple[float, float, float, float]


@dataclass(frozen=True)
class SpectrumRecord:
    timestamp_ms: int
    target_levels_dbfs: tuple[float, float, float, float]
    rms_dbfs: float


class BaselineTracker:
    """Average baseline samples in linear power, never directly in decibels."""

    def __init__(self) -> None:
        self.collecting = False
        self.sample_count = 0
        self._power_sums = [0.0] * TARGET_COUNT
        self.levels_dbfs: tuple[float, float, float, float] | None = None

    def start(self) -> None:
        self.collecting = True
        self.sample_count = 0
        self._power_sums = [0.0] * TARGET_COUNT
        self.levels_dbfs = None

    def add(self, levels_dbfs: Sequence[float]) -> None:
        if not self.collecting:
            return
        for index, level_dbfs in enumerate(levels_dbfs):
            self._power_sums[index] += 10.0 ** (level_dbfs / 10.0)
        self.sample_count += 1

    def stop(self) -> tuple[float, float, float, float]:
        if not self.collecting:
            raise ValueError("baseline collection has not been started")
        if self.sample_count == 0:
            raise ValueError("no spectrum samples were received during the baseline")

        self.collecting = False
        averaged = [
            10.0 * math.log10(power_sum / self.sample_count)
            for power_sum in self._power_sums
        ]
        self.levels_dbfs = tuple(averaged)  # type: ignore[assignment]
        return self.levels_dbfs

    def deltas(self, levels_dbfs: Sequence[float]) -> tuple[float, ...] | None:
        if self.levels_dbfs is None:
            return None
        return tuple(
            current - baseline
            for current, baseline in zip(levels_dbfs, self.levels_dbfs)
        )


def parse_audio_config(line: str) -> AudioConfig | None:
    fields = line.strip().split(",")
    if len(fields) != 8 or fields[0] != "AUDIO_CONFIG":
        return None
    try:
        sample_rate_hz = int(fields[1])
        fft_size = int(fields[2])
        half_bandwidth_hz = float(fields[3])
        targets = tuple(float(value) for value in fields[4:8])
    except ValueError:
        return None
    if (
        sample_rate_hz <= 0
        or fft_size <= 0
        or half_bandwidth_hz <= 0.0
        or len(targets) != TARGET_COUNT
        or not all(math.isfinite(value) for value in targets)
    ):
        return None
    return AudioConfig(
        sample_rate_hz=sample_rate_hz,
        fft_size=fft_size,
        half_bandwidth_hz=half_bandwidth_hz,
        target_frequencies_hz=targets,  # type: ignore[arg-type]
    )


def parse_spectrum_record(line: str) -> SpectrumRecord | None:
    fields = line.strip().split(",")
    if len(fields) != 7 or fields[0] != "SPECTRUM":
        return None
    try:
        timestamp_ms = int(fields[1])
        values = tuple(float(value) for value in fields[2:7])
    except ValueError:
        return None
    if timestamp_ms < 0 or not all(math.isfinite(value) for value in values):
        return None
    return SpectrumRecord(
        timestamp_ms=timestamp_ms,
        target_levels_dbfs=values[:4],  # type: ignore[arg-type]
        rms_dbfs=values[4],
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Plot the four target-frequency dBFS levels and compare them with "
            "a locally measured baseline."
        )
    )
    parser.add_argument(
        "--port",
        help="serial port; omitted to auto-detect one connected Espressif device",
    )
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument(
        "--history-seconds",
        type=float,
        default=60.0,
        help="rolling plot duration (default: 60)",
    )
    args = parser.parse_args()
    if args.baud <= 0:
        parser.error("--baud must be positive")
    if args.history_seconds <= 0:
        parser.error("--history-seconds must be positive")
    return args


def import_runtime_dependencies():
    try:
        import matplotlib.pyplot as plt
        from matplotlib.animation import FuncAnimation
        import serial
        from serial.tools import list_ports
    except ImportError:
        print(
            "Plotting dependencies are required. Install them with:\n"
            "  python3 -m pip install -r tools/requirements.txt",
            file=sys.stderr,
        )
        raise SystemExit(2) from None
    return plt, FuncAnimation, serial, list_ports


def select_port(requested_port: str | None, list_ports) -> str:
    if requested_port:
        return requested_port

    ports = list(list_ports.comports())
    espressif_ports = [port for port in ports if port.vid == ESPRESSIF_USB_VENDOR_ID]
    if len(espressif_ports) == 1:
        return espressif_ports[0].device

    detected = "\n".join(
        f"  {port.device}: {port.description}" for port in ports
    ) or "  (none)"
    reason = "none were" if not espressif_ports else "more than one was"
    raise RuntimeError(
        f"Could not auto-select a serial port because {reason} detected.\n"
        f"Detected ports:\n{detected}\n"
        "Close idf.py monitor, then specify the board with --port."
    )


def wait_for_prompt(connection, timeout_seconds: float = 10.0) -> None:
    deadline = time.monotonic() + timeout_seconds
    response = bytearray()
    while time.monotonic() < deadline:
        chunk = connection.read(connection.in_waiting or 1)
        if chunk:
            response.extend(chunk)
            if SERIAL_PROMPT in response:
                return
    raise TimeoutError("timed out waiting for the firmware's 'servo>' prompt")


def wait_for_audio_config(connection, timeout_seconds: float = 5.0) -> AudioConfig:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        raw_line = connection.readline()
        if not raw_line:
            continue
        config = parse_audio_config(raw_line.decode(errors="replace"))
        if config is not None:
            return config
    raise TimeoutError("timed out waiting for the AUDIO_CONFIG record")


def send_command(connection, write_lock: threading.Lock, command: str) -> None:
    with write_lock:
        connection.write((command + "\r").encode())
        connection.flush()


def serial_reader(connection, output: queue.Queue[str], stop: threading.Event) -> None:
    while not stop.is_set():
        try:
            raw_line = connection.readline()
        except OSError as error:
            output.put(f"SERIAL_ERROR,{error}")
            stop.set()
            return
        if raw_line:
            output.put(raw_line.decode(errors="replace").strip())


def terminal_reader(commands: queue.Queue[str], stop: threading.Event) -> None:
    while not stop.is_set():
        try:
            command = input("command> ").strip()
        except (EOFError, KeyboardInterrupt):
            commands.put("quit")
            return
        if command:
            commands.put(command)


def run_plot(connection, config: AudioConfig, history_seconds: float, plt, FuncAnimation) -> None:
    serial_lines: queue.Queue[str] = queue.Queue()
    commands: queue.Queue[str] = queue.Queue()
    stop = threading.Event()
    write_lock = threading.Lock()
    baseline = BaselineTracker()

    reader = threading.Thread(
        target=serial_reader,
        args=(connection, serial_lines, stop),
        daemon=True,
        name="serial-reader",
    )
    terminal = threading.Thread(
        target=terminal_reader,
        args=(commands, stop),
        daemon=True,
        name="terminal-input",
    )
    reader.start()
    terminal.start()

    colors = ("tab:blue", "tab:orange", "tab:green", "tab:red")
    elapsed_history: deque[float] = deque()
    absolute_history = [deque() for _ in range(TARGET_COUNT)]
    delta_time_history: deque[float] = deque()
    delta_history = [deque() for _ in range(TARGET_COUNT)]
    event_markers: list[tuple[float, str]] = []
    first_timestamp_ms: int | None = None
    latest_elapsed = 0.0
    latest_record: SpectrumRecord | None = None

    figure, (absolute_axis, delta_axis) = plt.subplots(
        2, 1, sharex=True, figsize=(11, 8), constrained_layout=True
    )
    figure.canvas.manager.set_window_title("AMM Live Acoustic Response")
    absolute_lines = []
    delta_lines = []
    baseline_lines = []
    for index, (frequency, color) in enumerate(
        zip(config.target_frequencies_hz, colors)
    ):
        label = f"{frequency:g} Hz"
        absolute_line, = absolute_axis.plot([], [], color=color, label=label)
        delta_line, = delta_axis.plot([], [], color=color, label=label)
        baseline_line = absolute_axis.axhline(
            0.0, color=color, linestyle="--", alpha=0.0
        )
        absolute_lines.append(absolute_line)
        delta_lines.append(delta_line)
        baseline_lines.append(baseline_line)

    absolute_axis.set_title(
        "Target-band levels (relative dBFS, not calibrated dB SPL)"
    )
    absolute_axis.set_ylabel("Level (dBFS)")
    absolute_axis.set_ylim(-120.0, 0.0)
    absolute_axis.grid(True, alpha=0.3)
    absolute_axis.legend(loc="lower left", ncol=4)
    delta_axis.set_title("Change relative to frozen baseline")
    delta_axis.set_ylabel("Change (dB)")
    delta_axis.set_xlabel("Time since first sample (s)")
    delta_axis.grid(True, alpha=0.3)
    delta_axis.axhline(0.0, color="black", linewidth=0.8)
    delta_axis.legend(loc="lower left", ncol=4)
    current_text = absolute_axis.text(
        0.99,
        0.98,
        "Waiting for data…",
        transform=absolute_axis.transAxes,
        horizontalalignment="right",
        verticalalignment="top",
        family="monospace",
        bbox={"facecolor": "white", "alpha": 0.8, "edgecolor": "0.7"},
    )
    baseline_status = delta_axis.text(
        0.99,
        0.98,
        "Baseline: not set",
        transform=delta_axis.transAxes,
        horizontalalignment="right",
        verticalalignment="top",
        bbox={"facecolor": "white", "alpha": 0.8, "edgecolor": "0.7"},
    )

    print(
        "\nPlot is running. Terminal commands:\n"
        "  baseline start   begin collecting a baseline\n"
        "  baseline stop    freeze the power-averaged baseline\n"
        "  set/all/center   forwarded to the firmware and marked on the plot\n"
        "  status, level, spectrum, stream on/off, help\n"
        "  quit             close the plot and serial connection\n"
    )

    def handle_terminal_command(command: str) -> None:
        nonlocal latest_elapsed
        lowered = command.lower()
        if lowered == "baseline start":
            baseline.start()
            delta_time_history.clear()
            for history in delta_history:
                history.clear()
            print("Baseline collection started.")
            return
        if lowered == "baseline stop":
            try:
                levels = baseline.stop()
            except ValueError as error:
                print(f"Cannot stop baseline: {error}")
                return
            formatted = ", ".join(
                f"{frequency:g} Hz={level:.2f} dBFS"
                for frequency, level in zip(
                    config.target_frequencies_hz, levels
                )
            )
            print(f"Baseline frozen: {formatted}")
            return
        if lowered == "quit":
            stop.set()
            plt.close(figure)
            return

        command_name = lowered.split(maxsplit=1)[0]
        if command_name not in FORWARDED_COMMANDS:
            print(
                "Unknown local command. Use baseline start/stop, quit, or a "
                "firmware console command."
            )
            return
        send_command(connection, write_lock, command)
        if command_name in ACTUATION_COMMANDS:
            event_markers.append((latest_elapsed, command))

    def update_plot(_frame):
        nonlocal first_timestamp_ms, latest_elapsed, latest_record

        while True:
            try:
                command = commands.get_nowait()
            except queue.Empty:
                break
            handle_terminal_command(command)

        while True:
            try:
                line = serial_lines.get_nowait()
            except queue.Empty:
                break
            if line.startswith("SERIAL_ERROR,"):
                print(line.replace("SERIAL_ERROR,", "Serial connection failed: ", 1))
                stop.set()
                plt.close(figure)
                break

            record = parse_spectrum_record(line)
            if record is None:
                if line and not line.startswith(("FFT,", "FFT_BEGIN,", "FFT_END,")):
                    print(line)
                continue

            if first_timestamp_ms is None:
                first_timestamp_ms = record.timestamp_ms
            latest_elapsed = (record.timestamp_ms - first_timestamp_ms) / 1000.0
            latest_record = record
            elapsed_history.append(latest_elapsed)
            for index, value in enumerate(record.target_levels_dbfs):
                absolute_history[index].append(value)

            baseline.add(record.target_levels_dbfs)
            deltas = baseline.deltas(record.target_levels_dbfs)
            if deltas is not None:
                delta_time_history.append(latest_elapsed)
                for index, value in enumerate(deltas):
                    delta_history[index].append(value)

        cutoff = latest_elapsed - history_seconds
        while elapsed_history and elapsed_history[0] < cutoff:
            elapsed_history.popleft()
            for history in absolute_history:
                history.popleft()
        while delta_time_history and delta_time_history[0] < cutoff:
            delta_time_history.popleft()
            for history in delta_history:
                history.popleft()

        for index in range(TARGET_COUNT):
            absolute_lines[index].set_data(elapsed_history, absolute_history[index])
            delta_lines[index].set_data(delta_time_history, delta_history[index])
            if baseline.levels_dbfs is None:
                baseline_lines[index].set_alpha(0.0)
            else:
                baseline_lines[index].set_ydata(
                    [baseline.levels_dbfs[index], baseline.levels_dbfs[index]]
                )
                baseline_lines[index].set_alpha(0.7)

        if latest_record is not None:
            deltas = baseline.deltas(latest_record.target_levels_dbfs)
            rows = []
            for index, (frequency, level) in enumerate(
                zip(
                    config.target_frequencies_hz,
                    latest_record.target_levels_dbfs,
                )
            ):
                suffix = "" if deltas is None else f"  Δ {deltas[index]:+6.2f} dB"
                rows.append(f"{frequency:6g} Hz  {level:7.2f} dBFS{suffix}")
            rows.append(f"RMS       {latest_record.rms_dbfs:7.2f} dBFS")
            current_text.set_text("\n".join(rows))

        if baseline.collecting:
            baseline_status.set_text(
                f"Baseline: collecting ({baseline.sample_count} samples)"
            )
        elif baseline.levels_dbfs is not None:
            baseline_status.set_text("Baseline: frozen")
        else:
            baseline_status.set_text("Baseline: not set")

        left = max(0.0, latest_elapsed - history_seconds)
        right = max(history_seconds, latest_elapsed + 1.0)
        absolute_axis.set_xlim(left, right)
        if delta_time_history:
            delta_axis.relim()
            delta_axis.autoscale_view(scalex=False, scaley=True)
            lower, upper = delta_axis.get_ylim()
            if upper - lower < 2.0:
                midpoint = (lower + upper) / 2.0
                delta_axis.set_ylim(midpoint - 1.0, midpoint + 1.0)
        else:
            delta_axis.set_ylim(-10.0, 10.0)

        for event_time, label in event_markers:
            if event_time >= cutoff:
                absolute_axis.axvline(event_time, color="0.3", alpha=0.25)
                delta_axis.axvline(event_time, color="0.3", alpha=0.25)
                absolute_axis.annotate(
                    label,
                    xy=(event_time, 0.02),
                    xycoords=("data", "axes fraction"),
                    rotation=90,
                    fontsize=8,
                )
        event_markers.clear()

        return (
            *absolute_lines,
            *delta_lines,
            *baseline_lines,
            current_text,
            baseline_status,
        )

    animation = FuncAnimation(
        figure, update_plot, interval=100, blit=False, cache_frame_data=False
    )
    # Keep a strong reference for Matplotlib backends that otherwise collect it.
    figure._amm_animation = animation  # type: ignore[attr-defined]
    try:
        plt.show()
    finally:
        stop.set()
        try:
            send_command(connection, write_lock, "stream off")
        except OSError:
            pass


def main() -> int:
    args = parse_args()
    plt, FuncAnimation, serial, list_ports = import_runtime_dependencies()
    try:
        port = select_port(args.port, list_ports)
    except RuntimeError as error:
        print(error, file=sys.stderr)
        return 2

    print(
        "WARNING: Opening the serial port may reset the ESP32 and command all "
        "servos to their configured 0-degree startup position.\n"
        "Close idf.py monitor before continuing."
    )

    try:
        with serial.Serial(
            port=port,
            baudrate=args.baud,
            timeout=0.2,
            write_timeout=1.0,
        ) as connection:
            connection.dtr = False
            connection.rts = False
            time.sleep(1.0)
            connection.reset_input_buffer()
            connection.write(b"\r")
            connection.flush()
            wait_for_prompt(connection)
            connection.reset_input_buffer()
            connection.write(b"stream on\r")
            connection.flush()
            config = wait_for_audio_config(connection)
            run_plot(connection, config, args.history_seconds, plt, FuncAnimation)
    except (OSError, TimeoutError, serial.SerialException) as error:
        print(f"Live spectrum stopped: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
