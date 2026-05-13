"""
KnockKnock - Cloud Backend Entry Point.

Connects to the Mosquitto MQTT broker, subscribes to edge responses, and
presents an interactive CLI for managing edge devices.
"""

from __future__ import annotations

import logging
import sys
import threading
import time
from typing import Optional

import paho.mqtt.client as mqtt

from .device_manager import Device, DeviceManager
from .message_types import (
    DeviceAckPayload,
    DeviceInfo,
    DeviceListResponsePayload,
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
EDGE_ID = "+"  # wildcard – we listen for all edges

TOPIC_COMMAND = "knockknock/edge/{edge_id}/command"
TOPIC_RESPONSE = f"knockknock/edge/{EDGE_ID}/response"
TOPIC_STATUS = f"knockknock/edge/{EDGE_ID}/status"

# Used as default edge_id when sending commands from CLI.
DEFAULT_EDGE_ID = "esp32-01"

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
    datefmt="%Y-%m-%dT%H:%M:%S",
)
logger = logging.getLogger("knockknock.backend")

# ---------------------------------------------------------------------------
# Global state
# ---------------------------------------------------------------------------

device_manager = DeviceManager()
_client: Optional[mqtt.Client] = None
_running = True


# ---------------------------------------------------------------------------
# MQTT Callbacks
# ---------------------------------------------------------------------------


def on_connect(client: mqtt.Client, userdata, flags, reason_code, properties) -> None:
    """Callback when the MQTT client connects (or reconnects) to the broker."""
    if reason_code == 0:
        logger.info("Connected to MQTT broker at %s:%d", BROKER_HOST, BROKER_PORT)
        client.subscribe(TOPIC_RESPONSE, qos=1)
        client.subscribe(TOPIC_STATUS, qos=1)
        logger.info("Subscribed to %s and %s", TOPIC_RESPONSE, TOPIC_STATUS)
    else:
        logger.error("Failed to connect to MQTT broker.  Reason code: %d", reason_code)


def on_disconnect(
    client: mqtt.Client, userdata, flags, reason_code, properties
) -> None:
    """Callback when the client disconnects from the broker."""
    if reason_code != 0:
        logger.warning(
            "Unexpected disconnect from MQTT broker (rc=%d).  Will auto-reconnect.",
            reason_code,
        )
    else:
        logger.info("Disconnected from MQTT broker.")


def on_message(client: mqtt.Client, userdata, msg: mqtt.MQTTMessage) -> None:
    """Callback for every inbound MQTT message."""
    logger.info("MQTT message received on topic '%s'", msg.topic)

    message = deserialize_message(msg.payload)
    if message is None:
        logger.warning("Ignoring malformed message on topic '%s'", msg.topic)
        return

    handler = _HANDLERS.get(message.type)
    if handler is not None:
        try:
            handler(message.payload)
        except Exception:
            logger.exception(
                "Unhandled error while processing '%s' message", message.type.value
            )
    else:
        logger.info("No handler registered for message type '%s'", message.type.value)


# ---------------------------------------------------------------------------
# Message Handlers (run on the MQTT network thread)
# ---------------------------------------------------------------------------


def _handle_device_list_response(payload: dict) -> None:
    """Handle a DEVICE_LIST_RESPONSE from the edge."""
    resp = DeviceListResponsePayload(devices=payload.get("devices", []))
    devices = resp.to_device_infos()
    logger.info("Device list response from edge: %d device(s)", len(devices))
    print("\n--- Edge Device List ---")
    if not devices:
        print("  (no devices)")
    else:
        for d in devices:
            print(f"  {d.mac_address}  {d.device_name:20s}  [{d.status}]")
    print("------------------------\n")


def _handle_device_ack(payload: dict) -> None:
    """Handle a DEVICE_ACK from the edge."""
    ack = DeviceAckPayload(
        action=payload.get("action", "unknown"),
        mac_address=payload.get("mac_address", ""),
        success=payload.get("success", False),
        message=payload.get("message"),
    )
    status = "SUCCESS" if ack.success else "FAILURE"
    logger.info(
        "ACK from edge: action=%s mac=%s %s (%s)",
        ack.action,
        ack.mac_address,
        status,
        ack.message or "",
    )
    print(f"\n[Edge ACK] {ack.action} {ack.mac_address} -> {status}")
    if ack.message:
        print(f"  Message: {ack.message}")


_HANDLERS = {
    MessageType.DEVICE_LIST_RESPONSE: _handle_device_list_response,
    MessageType.DEVICE_ACK: _handle_device_ack,
}


# ---------------------------------------------------------------------------
# MQTT publish helper
# ---------------------------------------------------------------------------


def publish_message(topic: str, message, qos: int = 1) -> None:
    """Publish a ``Message`` envelope to the given topic."""
    if _client is None:
        logger.error("MQTT client is not initialised; cannot publish.")
        return

    payload = serialize_message(message)
    result = _client.publish(topic, payload, qos=qos)

    if result.rc == mqtt.MQTT_ERR_SUCCESS:
        logger.info("Published to '%s': %s", topic, payload)
    else:
        logger.error("Failed to publish to '%s' (rc=%d)", topic, result.rc)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def _cli_input(prompt: str = "> ") -> str:
    """Wrapper around :func:`input` for testability."""
    return input(prompt)


def _print_menu() -> None:
    """Display the main menu."""
    print("\n" + "=" * 50)
    print("  KnockKnock - Cloud Backend CLI")
    print("=" * 50)
    print("  1. List known devices (local cache)")
    print("  2. Add a device")
    print("  3. Remove a device")
    print("  4. Request device list from edge")
    print("  5. Quit")
    print("-" * 50)


def _cmd_list_devices() -> None:
    """Print devices stored in the local :class:`DeviceManager`."""
    devices = device_manager.get_devices()
    print("\n--- Local Device Cache ---")
    if not devices:
        print("  (no devices)")
    else:
        for d in devices:
            print(f"  {d.mac_address}  {d.device_name:20s}  [{d.status}]")
    print("--------------------------")


def _cmd_add_device() -> None:
    """Prompt the user for a MAC address and optional name, then send an
    ADD_DEVICE command to the edge."""
    mac = _cli_input("  MAC address (e.g. AA:BB:CC:DD:EE:FF): ").strip()
    if not mac:
        print("  MAC address is required.")
        return

    name = _cli_input("  Device name (optional): ").strip()

    # Register locally first.
    device_manager.add_device(mac, name or None)

    # Build and publish the MQTT message.
    message = build_add_device_message(mac, name or None)
    topic = TOPIC_COMMAND.format(edge_id=DEFAULT_EDGE_ID)
    publish_message(topic, message)


def _cmd_remove_device() -> None:
    """Prompt the user for a MAC address, then send a REMOVE_DEVICE command
    to the edge."""
    mac = _cli_input("  MAC address to remove: ").strip()
    if not mac:
        print("  MAC address is required.")
        return

    if not device_manager.remove_device(mac):
        print(f"  Warning: {mac} was not in the local cache.  Sending command anyway.")

    message = build_remove_device_message(mac)
    topic = TOPIC_COMMAND.format(edge_id=DEFAULT_EDGE_ID)
    publish_message(topic, message)


def _cmd_request_device_list() -> None:
    """Send a DEVICE_LIST_REQUEST to the edge and wait briefly for the
    response to arrive."""
    message = build_device_list_request_message()
    topic = TOPIC_COMMAND.format(edge_id=DEFAULT_EDGE_ID)
    publish_message(topic, message)
    print("  Request sent.  Waiting for response (press Enter to return to menu)...")
    # Give the network thread a moment to receive the response.
    time.sleep(1.5)


def _cli_loop() -> None:
    """Main interactive loop."""
    commands = {
        "1": ("List devices", _cmd_list_devices),
        "2": ("Add device", _cmd_add_device),
        "3": ("Remove device", _cmd_remove_device),
        "4": ("Request device list", _cmd_request_device_list),
        "5": ("Quit", None),
    }

    while _running:
        _print_menu()
        choice = _cli_input("  Choice [1-5]: ").strip()

        if choice == "5":
            print("  Shutting down...")
            break

        handler = commands.get(choice)
        if handler is not None:
            _, func = handler
            try:
                func()
            except Exception:
                logger.exception("Error executing command '%s'", choice)
                print(f"  An unexpected error occurred.  See logs for details.")
        else:
            print(f"  Invalid choice: {choice!r}")


# ---------------------------------------------------------------------------
# Entry Point
# ---------------------------------------------------------------------------


def main() -> None:
    """Initialise the MQTT client and start the CLI."""
    global _client, _running

    logger.info("KnockKnock Cloud Backend starting...")
    logger.info("Broker: %s:%d", BROKER_HOST, BROKER_PORT)

    # --- MQTT Client setup ---
    _client = mqtt.Client(client_id="knockknock-cloud-backend", protocol=mqtt.MQTTv311)
    _client.on_connect = on_connect
    _client.on_disconnect = on_disconnect
    _client.on_message = on_message

    # Enable automatic reconnect.
    _client.reconnect_delay_set(min_delay=1, max_delay=30)

    try:
        _client.connect(BROKER_HOST, BROKER_PORT, keepalive=60)
    except ConnectionRefusedError:
        logger.error(
            "Could not connect to MQTT broker at %s:%d.  Is the mosquitto service running?",
            BROKER_HOST,
            BROKER_PORT,
        )
        sys.exit(1)

    # Start the MQTT network loop in a background thread.
    _client.loop_start()

    print("\nConnected to MQTT broker.  Starting CLI...\n")

    try:
        _cli_loop()
    except KeyboardInterrupt:
        print("\n  Interrupted by user.")
    finally:
        _running = False
        logger.info("Stopping MQTT client...")
        _client.loop_stop()
        _client.disconnect()
        logger.info("Shutdown complete.")


if __name__ == "__main__":
    main()
