"""
KnockKnock - Message Type Definitions.

Defines the message envelope and payload types used for communication
between the cloud backend and the edge hub (ESP32) over MQTT.
"""

from __future__ import annotations

import json
import logging
from dataclasses import asdict, dataclass, field
from datetime import datetime, timezone
from enum import Enum
from typing import Any, Optional

logger = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# Message Type Enum
# ---------------------------------------------------------------------------


class MessageType(str, Enum):
    """String-based enum of all recognised message types on the KnockKnock
    MQTT channel."""

    ADD_DEVICE = "ADD_DEVICE"
    REMOVE_DEVICE = "REMOVE_DEVICE"
    DEVICE_LIST_REQUEST = "DEVICE_LIST_REQUEST"
    DEVICE_LIST_RESPONSE = "DEVICE_LIST_RESPONSE"
    DEVICE_ACK = "DEVICE_ACK"


# ---------------------------------------------------------------------------
# Payload Dataclasses
# ---------------------------------------------------------------------------


@dataclass
class AddDevicePayload:
    """Payload for adding a new BLE sensor to the edge hub."""

    mac_address: str
    device_name: Optional[str] = None


@dataclass
class RemoveDevicePayload:
    """Payload for removing a sensor from the edge hub."""

    mac_address: str


@dataclass
class DeviceInfo:
    """Represents a single device in the device-list response."""

    mac_address: str
    device_name: str
    status: str  # e.g. "online", "offline", "unknown"


@dataclass
class DeviceListResponsePayload:
    """Payload sent by the edge hub in response to a DEVICE_LIST_REQUEST."""

    devices: list[dict[str, str]] = field(default_factory=list)

    @classmethod
    def from_device_infos(cls, infos: list[DeviceInfo]) -> DeviceListResponsePayload:
        """Create payload from a list of DeviceInfo objects."""
        return cls(devices=[asdict(info) for info in infos])

    def to_device_infos(self) -> list[DeviceInfo]:
        """Convert the raw dicts back into DeviceInfo objects.

        Handles both cloud canonical field names and edge compact names:
          - mac / mac_address
          - name / device_name
          - active (bool) / status (str)
        """
        result: list[DeviceInfo] = []
        for d in self.devices:
            mac = d.get("mac_address") or d.get("mac", "")
            name = d.get("device_name") or d.get("name", "")
            # Edge sends 'active' as a boolean; map to status string
            if "status" in d:
                status = d["status"]
            elif d.get("active", False):
                status = "online"
            else:
                status = "offline"
            result.append(DeviceInfo(mac_address=mac, device_name=name, status=status))
        return result


@dataclass
class DeviceAckPayload:
    """Acknowledgment sent by the edge hub after processing a command."""

    action: str
    mac_address: str
    success: bool
    message: Optional[str] = None


# ---------------------------------------------------------------------------
# Top-Level Message Envelope
# ---------------------------------------------------------------------------


@dataclass
class Message:
    """Universal message envelope for all KnockKnock MQTT communication.

    Schema:
        {
            "type": "<MessageType value>",
            "payload": { ... },
            "timestamp": "<ISO-8601 UTC string>"
        }
    """

    type: MessageType
    payload: dict[str, Any] = field(default_factory=dict)
    timestamp: str = field(
        default_factory=lambda: datetime.now(timezone.utc).isoformat()
    )


# ---------------------------------------------------------------------------
# Serialization Helpers
# ---------------------------------------------------------------------------


def serialize_message(message: Message) -> str:
    """Serialize a Message envelope to a JSON string."""
    return json.dumps(
        {
            "type": message.type.value,
            "payload": message.payload,
            "timestamp": message.timestamp,
        },
        ensure_ascii=False,
    )


def deserialize_message(raw: str | bytes) -> Optional[Message]:
    """Parse a JSON string (or bytes) into a Message envelope.

    Handles two formats:
    1. Cloud envelope: {"type":"...", "payload":{...}, "timestamp":"..."}
    2. Edge flat format: {"type":"...", "action":"...", "mac":"...", ...}
       (the entire object minus 'type' becomes the payload)

    Returns ``None`` if the input is malformed or missing required fields.
    """
    try:
        if isinstance(raw, bytes):
            raw = raw.decode("utf-8")

        data: dict[str, Any] = json.loads(raw)

        msg_type = MessageType(data["type"])
        timestamp: str = data.get("timestamp", "")

        # If the message has a 'payload' key, it's the cloud envelope format
        if "payload" in data and isinstance(data["payload"], dict):
            payload: dict[str, Any] = data["payload"]
        else:
            # Edge flat format: everything except 'type' and 'timestamp' is payload
            payload = {k: v for k, v in data.items() if k not in ("type", "timestamp")}
            # Normalise field names that the edge shortens
            _normalise_edge_fields(payload)

        return Message(type=msg_type, payload=payload, timestamp=timestamp)
    except (json.JSONDecodeError, KeyError, ValueError) as exc:
        logger.warning("Failed to deserialize message: %s  Raw=%s", exc, raw)
        return None


def _normalise_edge_fields(payload: dict[str, Any]) -> None:
    """Normalise field names from the edge hub's compact C format
    to the cloud's canonical names.  Mutates *payload* in place."""
    # 'mac' -> 'mac_address'
    if "mac" in payload and "mac_address" not in payload:
        payload["mac_address"] = payload.pop("mac")
    # 'name' -> 'device_name'
    if "name" in payload and "device_name" not in payload:
        payload["device_name"] = payload.pop("name", "")


# ---------------------------------------------------------------------------
# Convenience constructors — build a full Message from a typed payload
# ---------------------------------------------------------------------------


def build_add_device_message(
    mac_address: str, device_name: Optional[str] = None
) -> Message:
    """Create an ADD_DEVICE message envelope."""
    payload = AddDevicePayload(mac_address=mac_address, device_name=device_name)
    return Message(type=MessageType.ADD_DEVICE, payload=asdict(payload))


def build_remove_device_message(mac_address: str) -> Message:
    """Create a REMOVE_DEVICE message envelope."""
    payload = RemoveDevicePayload(mac_address=mac_address)
    return Message(type=MessageType.REMOVE_DEVICE, payload=asdict(payload))


def build_device_list_request_message() -> Message:
    """Create a DEVICE_LIST_REQUEST message envelope (no payload)."""
    return Message(type=MessageType.DEVICE_LIST_REQUEST)


def build_device_list_response_message(devices: list[DeviceInfo]) -> Message:
    """Create a DEVICE_LIST_RESPONSE message envelope."""
    payload = DeviceListResponsePayload.from_device_infos(devices)
    return Message(type=MessageType.DEVICE_LIST_RESPONSE, payload=asdict(payload))


def build_device_ack_message(
    action: str,
    mac_address: str,
    success: bool,
    msg: Optional[str] = None,
) -> Message:
    """Create a DEVICE_ACK message envelope."""
    payload = DeviceAckPayload(
        action=action, mac_address=mac_address, success=success, message=msg
    )
    return Message(type=MessageType.DEVICE_ACK, payload=asdict(payload))
