"""Live dashboard: polls the ESP32's WiFi status API and streams state to the browser.

Read-only monitoring only — this never sends servo commands to the ESP32.
Run with: python -m visualization.server [--esp32-host http://192.168.4.1]
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
from pathlib import Path
from typing import Optional

import uvicorn
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles

from visualization.calibration import DepthCalibration, load_calibrations
from visualization.esp32_client import ControlUnitState, DEFAULT_BASE_URL, Esp32Client, SpectrumState

logger = logging.getLogger("visualization")

POLL_INTERVAL_SECONDS = 0.1
SERVO_COUNT = 4
# Only 2 of the firmware's 4 servo slots have a real cavity/piston wired up
# right now: servo 1 (GPIO36) and servo 2 (GPIO37). Kept in sync with
# REAL_CAVITY_IDS in visualization/static/app.js and visualization/autotune.py.
REAL_CAVITY_IDS = [1, 2]
STATIC_DIR = Path(__file__).resolve().parent / "static"

app = FastAPI()
app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")

_websocket_clients: set[WebSocket] = set()
_esp32_client: Optional[Esp32Client] = None
_calibrations: dict[int, DepthCalibration] = {}


@app.get("/")
async def index() -> FileResponse:
    return FileResponse(STATIC_DIR / "index.html")


@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket) -> None:
    await websocket.accept()
    _websocket_clients.add(websocket)
    try:
        while True:
            # The dashboard is read-only; incoming messages are only used to
            # detect the browser closing the connection.
            await websocket.receive_text()
    except WebSocketDisconnect:
        pass
    finally:
        _websocket_clients.discard(websocket)


def build_frame(state: ControlUnitState, spectrum: Optional[SpectrumState]) -> dict:
    audio = state.audio
    cavities = []
    for servo in state.servos:
        calibration = _calibrations.get(servo.id)
        cavities.append(
            {
                "id": servo.id,
                "gpio": servo.gpio,
                "angle_deg": servo.angle_deg,
                "depth_mm": calibration.depth_mm(servo.angle_deg) if calibration else None,
                "max_depth_mm": calibration.max_depth_mm if calibration else None,
                "target_hz": audio.targets_hz[servo.id - 1] if audio else None,
                "level_dbfs": audio.target_level_dbfs[servo.id - 1] if audio else None,
            }
        )

    return {
        "connected": state.connected,
        "cavities": cavities,
        "rms_dbfs": audio.rms_dbfs if audio else None,
        "spectrum": (
            {"bin_width_hz": spectrum.bin_width_hz, "dbfs": spectrum.dbfs}
            if spectrum is not None
            else None
        ),
    }


async def poll_loop() -> None:
    assert _esp32_client is not None
    while True:
        state, spectrum = await asyncio.gather(
            asyncio.to_thread(_esp32_client.fetch_state),
            asyncio.to_thread(_esp32_client.fetch_spectrum),
        )
        message = json.dumps(build_frame(state, spectrum))

        stale_clients = []
        for websocket in _websocket_clients:
            try:
                await websocket.send_text(message)
            except Exception:  # noqa: BLE001 - a dead socket must not stop the loop
                stale_clients.append(websocket)
        for websocket in stale_clients:
            _websocket_clients.discard(websocket)

        await asyncio.sleep(POLL_INTERVAL_SECONDS)


@app.on_event("startup")
async def start_poller() -> None:
    asyncio.create_task(poll_loop())


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--esp32-host",
        default=DEFAULT_BASE_URL,
        help="Base URL of the ESP32 status API (default: %(default)s)",
    )
    parser.add_argument("--host", default="127.0.0.1", help="Dashboard bind address")
    parser.add_argument("--port", type=int, default=8000, help="Dashboard port")
    return parser.parse_args()


def main() -> None:
    global _esp32_client, _calibrations
    logging.basicConfig(level=logging.INFO)

    args = parse_args()
    _esp32_client = Esp32Client(args.esp32_host)
    _calibrations = load_calibrations(SERVO_COUNT)
    missing = sorted(set(REAL_CAVITY_IDS) - _calibrations.keys())
    if missing:
        logger.warning(
            "No calibration data for the real cavity/cavities %s — depth will "
            "show as raw angle until calibration/servo_N.csv exists. See "
            "tools/servo_calibration.py.",
            missing,
        )

    uvicorn.run(app, host=args.host, port=args.port, log_level="info")


if __name__ == "__main__":
    main()
