"""HTTP client for the ESP32's WiFi status API (wifi_status_server component).

The ESP32 hosts its own access point; join it before running the dashboard.
Default AP gateway address is ESP-IDF's standard 192.168.4.1.
"""

from __future__ import annotations

import dataclasses

import requests

DEFAULT_BASE_URL = "http://192.168.4.1"
REQUEST_TIMEOUT_SECONDS = 1.0


@dataclasses.dataclass
class ServoState:
    id: int
    gpio: int
    angle_deg: int


@dataclasses.dataclass
class AudioState:
    timestamp_ms: int
    targets_hz: list[float]
    target_level_dbfs: list[float]
    rms_dbfs: float


@dataclasses.dataclass
class SpectrumState:
    timestamp_ms: int
    bin_width_hz: float
    dbfs: list[float]


@dataclasses.dataclass
class ControlUnitState:
    connected: bool
    servos: list[ServoState]
    audio: AudioState | None


class Esp32Client:
    """Polls the ESP32's /state and /spectrum endpoints over WiFi."""

    def __init__(self, base_url: str = DEFAULT_BASE_URL) -> None:
        self._base_url = base_url.rstrip("/")
        self._session = requests.Session()

    def fetch_state(self) -> ControlUnitState:
        try:
            response = self._session.get(
                f"{self._base_url}/state", timeout=REQUEST_TIMEOUT_SECONDS
            )
            response.raise_for_status()
            payload = response.json()
        except (requests.RequestException, ValueError):
            return ControlUnitState(connected=False, servos=[], audio=None)

        servos = [
            ServoState(id=item["id"], gpio=item["gpio"], angle_deg=item["angle_deg"])
            for item in payload.get("servos", [])
        ]
        audio_payload = payload.get("audio")
        audio = (
            AudioState(
                timestamp_ms=audio_payload["timestamp_ms"],
                targets_hz=audio_payload["targets_hz"],
                target_level_dbfs=audio_payload["target_level_dbfs"],
                rms_dbfs=audio_payload["rms_dbfs"],
            )
            if audio_payload
            else None
        )
        return ControlUnitState(connected=True, servos=servos, audio=audio)

    def set_servo_angle(self, servo_id: int, angle_deg: int) -> int | None:
        """POST /servo. Returns the angle actually applied after the
        firmware's own clamping, or None if the request failed."""
        try:
            response = self._session.post(
                f"{self._base_url}/servo",
                params={"id": servo_id, "angle_deg": int(round(angle_deg))},
                timeout=REQUEST_TIMEOUT_SECONDS,
            )
            response.raise_for_status()
            payload = response.json()
        except (requests.RequestException, ValueError):
            return None
        return payload.get("angle_deg")

    def fetch_spectrum(self) -> SpectrumState | None:
        try:
            response = self._session.get(
                f"{self._base_url}/spectrum", timeout=REQUEST_TIMEOUT_SECONDS
            )
            response.raise_for_status()
            payload = response.json()
        except (requests.RequestException, ValueError):
            return None
        if not payload:
            return None
        return SpectrumState(
            timestamp_ms=payload["timestamp_ms"],
            bin_width_hz=payload["bin_width_hz"],
            dbfs=payload["dbfs"],
        )
