"""
KnockKnock - Cloud Backend (FastAPI).

Bridges MQTT ↔ REST/SSE so a web app can:
  - POST /api/alarm/enable|disable
  - POST /api/training/start
  - GET  /api/devices
  - GET  /api/events  (SSE — real-time alarm + ACK notifications)
"""

from __future__ import annotations

import asyncio
import logging
import re
from contextlib import asynccontextmanager
from dataclasses import asdict
from pathlib import Path
from typing import Optional

import paho.mqtt.client as mqtt
from fastapi import FastAPI, Request
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse, StreamingResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, field_validator

from .device_manager import DeviceManager
from .message_types import (
    Message,
    MessageType,
    build_add_device_message,
    build_device_list_request_message,
    build_remove_device_message,
    deserialize_message,
    serialize_message,
)

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

BROKER_HOST = "mosquitto"
BROKER_PORT = 1883
DEFAULT_EDGE = "esp32-01"

TOPIC_COMMAND = "knockknock/edge/{edge_id}/command"
TOPIC_RESPONSE = "knockknock/edge/+/response"
TOPIC_STATUS = "knockknock/edge/+/status"

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
)
logger = logging.getLogger("knockknock.backend")

# ---------------------------------------------------------------------------
# Shared state
# ---------------------------------------------------------------------------

device_manager = DeviceManager()
_mqtt_client: Optional[mqtt.Client] = None
_loop: Optional[asyncio.AbstractEventLoop] = None
_sse_queues: list[asyncio.Queue] = []
_alarm_enabled: bool = True  # best-known state, updated via ALARM_ACK

# ---------------------------------------------------------------------------
# SSE broadcast (called on asyncio thread via call_soon_threadsafe)
# ---------------------------------------------------------------------------


def _broadcast(data: str) -> None:
    for q in _sse_queues:
        q.put_nowait(data)


# ---------------------------------------------------------------------------
# MQTT callbacks (run on paho background thread)
# ---------------------------------------------------------------------------


def _on_connect(
    client: mqtt.Client, userdata, flags, reason_code, properties=None
) -> None:
    if reason_code == 0:
        client.subscribe(TOPIC_RESPONSE, qos=1)
        client.subscribe(TOPIC_STATUS, qos=1)
        logger.info("MQTT connected, subscribed to response + status topics")
    else:
        logger.error("MQTT connect failed rc=%s", reason_code)


def _on_message(client: mqtt.Client, userdata, msg: mqtt.MQTTMessage) -> None:
    raw = msg.payload.decode("utf-8", errors="replace")
    logger.info("MQTT ← %s: %s", msg.topic, raw)

    # Push every inbound message to SSE clients so the webapp can react.
    if _loop:
        _loop.call_soon_threadsafe(_broadcast, raw)

    # Update local state from inbound messages.
    message = deserialize_message(msg.payload)
    if message is None:
        return
    if message.type == MessageType.DEVICE_LIST_RESPONSE:
        for d in message.payload.get("devices", []):
            mac = d.get("mac_address") or d.get("mac", "")
            name = d.get("device_name") or d.get("name", "")
            active = d.get("active", False)
            if mac:
                device_manager.add_device(mac, name or None)
                device_manager.update_device_status(mac, "online" if active else "offline")
    elif message.type == MessageType.ALARM_ACK:
        global _alarm_enabled
        _alarm_enabled = bool(message.payload.get("alarm_enabled", True))


# ---------------------------------------------------------------------------
# MQTT publish helper
# ---------------------------------------------------------------------------


def _publish(msg: Message) -> None:
    if _mqtt_client is None:
        logger.error("MQTT client not ready")
        return
    topic = TOPIC_COMMAND.format(edge_id=DEFAULT_EDGE)
    payload = serialize_message(msg)
    result = _mqtt_client.publish(topic, payload, qos=1)
    if result.rc != mqtt.MQTT_ERR_SUCCESS:
        logger.error("MQTT publish failed rc=%d", result.rc)
    else:
        logger.info("MQTT → %s: %s", topic, payload)


# ---------------------------------------------------------------------------
# App lifespan: start / stop MQTT
# ---------------------------------------------------------------------------


@asynccontextmanager
async def lifespan(app: FastAPI):
    global _mqtt_client, _loop
    _loop = asyncio.get_running_loop()

    _mqtt_client = mqtt.Client(
        client_id="knockknock-backend",
        protocol=mqtt.MQTTv311,
    )
    _mqtt_client.on_connect = _on_connect
    _mqtt_client.on_message = _on_message
    _mqtt_client.reconnect_delay_set(min_delay=1, max_delay=30)

    try:
        _mqtt_client.connect(BROKER_HOST, BROKER_PORT, keepalive=60)
    except Exception as exc:
        logger.error("Cannot connect to MQTT broker: %s", exc)

    _mqtt_client.loop_start()
    logger.info("Backend started — MQTT broker %s:%d", BROKER_HOST, BROKER_PORT)

    yield  # app runs here

    _mqtt_client.loop_stop()
    _mqtt_client.disconnect()
    logger.info("Backend stopped")


# ---------------------------------------------------------------------------
# FastAPI app
# ---------------------------------------------------------------------------

app = FastAPI(title="KnockKnock Backend", lifespan=lifespan)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

_STATIC_DIR = Path(__file__).parent / "static"


@app.get("/", include_in_schema=False)
async def index():
    return FileResponse(_STATIC_DIR / "index.html")


# ---------------------------------------------------------------------------
# SSE endpoint
# ---------------------------------------------------------------------------


@app.get("/api/events")
async def sse(request: Request):
    """
    Server-Sent Events stream.  Connect from the webapp with:
        const es = new EventSource('/api/events');
        es.onmessage = e => console.log(JSON.parse(e.data));
    Every inbound MQTT message (alarm, ACK, …) is forwarded here.
    """
    q: asyncio.Queue = asyncio.Queue()
    _sse_queues.append(q)

    async def generate():
        try:
            while True:
                try:
                    data = await asyncio.wait_for(q.get(), timeout=25.0)
                    yield f"data: {data}\n\n"
                except asyncio.TimeoutError:
                    yield ": keepalive\n\n"  # prevent proxy timeout
        except asyncio.CancelledError:
            pass
        finally:
            if q in _sse_queues:
                _sse_queues.remove(q)

    return StreamingResponse(generate(), media_type="text/event-stream")


# ---------------------------------------------------------------------------
# Alarm control
# ---------------------------------------------------------------------------


@app.get("/api/alarm/state")
async def alarm_state():
    return {"alarm_enabled": _alarm_enabled}


@app.post("/api/alarm/enable")
async def alarm_enable():
    _publish(Message(type=MessageType.ALARM_ENABLE))
    return {"ok": True}


@app.post("/api/alarm/disable")
async def alarm_disable():
    _publish(Message(type=MessageType.ALARM_DISABLE))
    return {"ok": True}


# ---------------------------------------------------------------------------
# Training
# ---------------------------------------------------------------------------

_MAC_RE = re.compile(
    r"^([0-9A-Fa-f]{2}[:\-]){5}[0-9A-Fa-f]{2}$"
    r"|^[0-9A-Fa-f]{12}$"
)


class RegisterSensorBody(BaseModel):
    mac_address: str
    device_name: str = ""

    @field_validator("mac_address")
    @classmethod
    def validate_mac(cls, v: str) -> str:
        normalised = v.upper().replace("-", ":").replace(".", ":").replace(" ", "")
        if ":" not in normalised and len(normalised) == 12:
            normalised = ":".join(normalised[i : i + 2] for i in range(0, 12, 2))
        if not _MAC_RE.match(normalised):
            raise ValueError(f"Invalid MAC address: {v!r}")
        return normalised


class StartTrainingBody(BaseModel):
    mac_address: str
    duration_ms: int = 60000


@app.post("/api/training/start")
async def training_start(body: StartTrainingBody):
    msg = Message(
        type=MessageType.START_TRAINING,
        payload=body.model_dump(),
    )
    _publish(msg)
    return {"ok": True}


# ---------------------------------------------------------------------------
# Device management
# ---------------------------------------------------------------------------


@app.get("/api/devices")
async def list_devices():
    return {"devices": [d.to_dict() for d in device_manager.get_devices()]}


@app.post("/api/devices", status_code=201)
async def add_device(body: RegisterSensorBody):
    """Register a new sensor in the cloud and push an ADD_DEVICE command to
    the edge hub via MQTT so the hub's NVS registry is updated immediately."""
    device_manager.add_device(body.mac_address, body.device_name or None)
    device_manager.update_device_status(body.mac_address, "offline")
    _publish(build_add_device_message(body.mac_address, body.device_name or None))
    logger.info(
        "Registered sensor %s ('%s') — ADD_DEVICE published to hub",
        body.mac_address,
        body.device_name,
    )
    return {"ok": True, "mac_address": body.mac_address}


@app.delete("/api/devices/{mac_address}")
async def remove_device(mac_address: str):
    device_manager.remove_device(mac_address)
    _publish(build_remove_device_message(mac_address))
    return {"ok": True}


@app.patch("/api/devices/{mac_address}")
async def rename_device(mac_address: str, device_name: str):
    device_manager.add_device(mac_address, device_name)
    _publish(build_add_device_message(mac_address, device_name))
    return {"ok": True}


@app.post("/api/devices/sync")
async def sync_devices():
    """Request the live device list from the edge hub."""
    _publish(build_device_list_request_message())
    return {"ok": True}


# Static files — must be mounted last so /api/ routes take priority.
app.mount("/static", StaticFiles(directory=str(_STATIC_DIR)), name="static")
