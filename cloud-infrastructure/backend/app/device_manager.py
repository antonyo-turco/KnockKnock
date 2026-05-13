"""
KnockKnock - Device Manager.

Thread-safe, in-memory registry of known edge devices (BLE sensors).
"""

from __future__ import annotations

import logging
from threading import Lock
from typing import Any, Optional

logger = logging.getLogger(__name__)


class Device:
    """Represents a single device / BLE sensor known to the cloud."""

    __slots__ = ("mac_address", "device_name", "status")

    def __init__(
        self, mac_address: str, device_name: str = "", status: str = "unknown"
    ) -> None:
        self.mac_address: str = mac_address
        self.device_name: str = device_name
        self.status: str = status

    def to_dict(self) -> dict[str, str]:
        """Return a JSON-friendly dict representation."""
        return {
            "mac_address": self.mac_address,
            "device_name": self.device_name,
            "status": self.status,
        }

    def __repr__(self) -> str:
        return f"Device(mac={self.mac_address!r}, name={self.device_name!r}, status={self.status!r})"


class DeviceManager:
    """Thread-safe store for devices the cloud backend knows about.

    Typical usage::

        dm = DeviceManager()
        dm.add_device("AA:BB:CC:DD:EE:FF", "Living Room Sensor")
        dev = dm.get_device("AA:BB:CC:DD:EE:FF")
        dm.remove_device("AA:BB:CC:DD:EE:FF")
    """

    def __init__(self) -> None:
        self._devices: dict[str, Device] = {}
        self._lock = Lock()

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

    def add_device(self, mac_address: str, device_name: Optional[str] = None) -> Device:
        """Add (or update) a device in the registry.

        Returns the ``Device`` instance.
        """
        with self._lock:
            normalized_mac = self._normalise_mac(mac_address)
            if normalized_mac in self._devices:
                # Update existing device instead of silently ignoring.
                existing = self._devices[normalized_mac]
                if device_name is not None:
                    existing.device_name = device_name
                logger.info("Updated existing device: %s", existing)
                return existing

            device = Device(mac_address=normalized_mac, device_name=device_name or "")
            self._devices[normalized_mac] = device
            logger.info("Added device: %s", device)
            return device

    def remove_device(self, mac_address: str) -> bool:
        """Remove a device by MAC address.

        Returns ``True`` if the device was found and removed, ``False``
        otherwise.
        """
        with self._lock:
            normalized_mac = self._normalise_mac(mac_address)
            if normalized_mac in self._devices:
                del self._devices[normalized_mac]
                logger.info("Removed device: %s", normalized_mac)
                return True
            logger.warning("Attempted to remove unknown device: %s", normalized_mac)
            return False

    def get_device(self, mac_address: str) -> Optional[Device]:
        """Return a single device by MAC, or ``None`` if not found."""
        with self._lock:
            normalized_mac = self._normalise_mac(mac_address)
            return self._devices.get(normalized_mac)

    def get_devices(self) -> list[Device]:
        """Return a snapshot of all known devices."""
        with self._lock:
            return list(self._devices.values())

    def update_device_status(self, mac_address: str, status: str) -> bool:
        """Update the status field of a device (e.g. 'online' / 'offline').

        Returns ``True`` if the device was found and updated.
        """
        with self._lock:
            normalized_mac = self._normalise_mac(mac_address)
            device = self._devices.get(normalized_mac)
            if device is None:
                logger.warning(
                    "Cannot update status for unknown device: %s", normalized_mac
                )
                return False
            device.status = status
            logger.info("Updated device %s status -> %s", normalized_mac, status)
            return True

    # ------------------------------------------------------------------
    # Helpers
    # ------------------------------------------------------------------

    @staticmethod
    def _normalise_mac(address: str) -> str:
        """Normalise a MAC address to uppercase with colons."""
        raw = address.upper().replace("-", ":").replace(".", ":").replace(" ", "")
        # Insert colons for un-delimited 12-char hex strings.
        if ":" not in raw and len(raw) == 12:
            raw = ":".join(raw[i : i + 2] for i in range(0, 12, 2))
        return raw
