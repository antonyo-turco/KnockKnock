#!/usr/bin/env python3
"""
setup_network.py — KnockKnock one-shot network setup.

Run this before building firmware whenever your LAN IP changes.

What it does:
  1. Detects your LAN IP automatically
  2. Generates TLS certs (CA + server) in pure Python — no openssl binary needed
  3. Copies ca.crt into the firmware tree (edge-hub/certs/)
  4. Patches CLOUD_MQTT_BROKER_IP in edge-hub/include/config.h
  5. Prints a QR code pointing at the backend API (http://<IP>:8000)
     — scan with your phone to open the webapp

Requirements:
  pip install cryptography qrcode
"""

import argparse
import datetime
import ipaddress
import re
import shutil
import socket
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# Paths (relative to this script = project root)
# ---------------------------------------------------------------------------

ROOT = Path(__file__).parent
BROKER_CA = ROOT / "cloud-infrastructure" / "mosquitto" / "certs" / "ca.crt"
FW_CA = ROOT / "edge-hub" / "certs" / "ca.crt"
CONFIG_H = ROOT / "edge-hub" / "include" / "config.h"

BACKEND_PORT = 3000

# ---------------------------------------------------------------------------
# Step 1: detect LAN IP
# ---------------------------------------------------------------------------


def get_lan_ip() -> str:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]


# ---------------------------------------------------------------------------
# Step 2: generate TLS certs in pure Python (cryptography library)
# ---------------------------------------------------------------------------


def generate_certs(ip: str) -> None:
    try:
        from cryptography import x509
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import rsa
        from cryptography.x509.oid import NameOID
    except ImportError:
        print("[ERROR] Missing dependency: pip install cryptography", file=sys.stderr)
        sys.exit(1)

    certs = BROKER_CA.parent
    certs.mkdir(parents=True, exist_ok=True)

    now = datetime.datetime.now(datetime.timezone.utc)
    expire = now + datetime.timedelta(days=3650)

    # --- CA key + self-signed cert ---
    print("[certs] Generating CA key and cert...")
    ca_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    ca_name = x509.Name(
        [
            x509.NameAttribute(NameOID.COMMON_NAME, "KnockKnock-CA"),
            x509.NameAttribute(NameOID.ORGANIZATION_NAME, "KnockKnock"),
        ]
    )
    ca_cert = (
        x509.CertificateBuilder()
        .subject_name(ca_name)
        .issuer_name(ca_name)
        .public_key(ca_key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now)
        .not_valid_after(expire)
        .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
        .sign(ca_key, hashes.SHA256())
    )

    # --- Server key + cert signed by CA ---
    # Include IP as SubjectAlternativeName — required by modern TLS (CN alone is deprecated for IPs)
    print(f"[certs] Generating server cert (IP SAN = {ip})...")
    srv_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    srv_name = x509.Name(
        [
            x509.NameAttribute(NameOID.COMMON_NAME, ip),
            x509.NameAttribute(NameOID.ORGANIZATION_NAME, "KnockKnock"),
        ]
    )
    srv_cert = (
        x509.CertificateBuilder()
        .subject_name(srv_name)
        .issuer_name(ca_name)
        .public_key(srv_key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now)
        .not_valid_after(expire)
        .add_extension(
            x509.SubjectAlternativeName(
                [
                    x509.IPAddress(ipaddress.IPv4Address(ip)),
                ]
            ),
            critical=False,
        )
        .sign(ca_key, hashes.SHA256())
    )

    # --- Write PEM files ---
    _pem = serialization.Encoding.PEM
    _no_enc = serialization.NoEncryption()
    _trad = serialization.PrivateFormat.TraditionalOpenSSL

    (certs / "ca.key").write_bytes(ca_key.private_bytes(_pem, _trad, _no_enc))
    (certs / "ca.crt").write_bytes(ca_cert.public_bytes(_pem))
    (certs / "server.key").write_bytes(srv_key.private_bytes(_pem, _trad, _no_enc))
    (certs / "server.crt").write_bytes(srv_cert.public_bytes(_pem))

    print(f"[certs] Done — written to {certs}")


# ---------------------------------------------------------------------------
# Step 3: copy ca.crt into firmware tree
# ---------------------------------------------------------------------------


def copy_ca_to_firmware() -> None:
    FW_CA.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(BROKER_CA, FW_CA)
    print(f"[firmware] ca.crt → {FW_CA.relative_to(ROOT)}")

    # Update ca_crt_embedded.c with the new certificate bytes
    embedded_c = ROOT / "edge-hub" / "src" / "ca_crt_embedded.c"
    if BROKER_CA.exists():
        data = BROKER_CA.read_bytes()
        hex_lines = []
        for i in range(0, len(data), 16):
            chunk = data[i:i+16]
            hex_lines.append("    " + ", ".join(f"0x{b:02x}" for b in chunk))
        hex_str = ",\n".join(hex_lines)
        content = f"""/* Auto-generated from certs/ca.crt — do not edit manually */
#include <stdint.h>

/* Null-terminated PEM — matches the symbol ESP-IDF EMBED_TXTFILES would generate */
const uint8_t _binary_certs_ca_crt_start[] = {{
{hex_str},
    0x00  /* null terminator */
}};

const uint8_t _binary_certs_ca_crt_end[] = {{ 0x00 }};
const uint32_t _binary_certs_ca_crt_length = sizeof(_binary_certs_ca_crt_start) - 1;
"""
        embedded_c.write_text(content, encoding="utf-8")
        print(f"[firmware] ca_crt_embedded.c updated")


# ---------------------------------------------------------------------------
# Step 4: read / patch config.h
# ---------------------------------------------------------------------------

_CONFIG_IP_RE = re.compile(r'#define\s+CLOUD_MQTT_BROKER_IP\s+"([^"]+)"')


def get_config_ip() -> str | None:
    """Return the IP currently written in config.h, or None if unreadable."""
    if not CONFIG_H.exists():
        return None
    m = _CONFIG_IP_RE.search(CONFIG_H.read_text(encoding="utf-8"))
    return m.group(1) if m else None


def patch_config_h(ip: str) -> None:
    text = CONFIG_H.read_text(encoding="utf-8")
    new_text = _CONFIG_IP_RE.sub(rf'#define CLOUD_MQTT_BROKER_IP "{ip}"', text)
    if new_text == text:
        print(
            "[config] WARNING: CLOUD_MQTT_BROKER_IP not found in config.h",
            file=sys.stderr,
        )
    else:
        CONFIG_H.write_text(new_text, encoding="utf-8")
        print(f"[config] CLOUD_MQTT_BROKER_IP = {ip}")


# ---------------------------------------------------------------------------
# Step 5: print QR code
# ---------------------------------------------------------------------------


def print_qr(ip: str) -> None:
    url = f"http://{ip}:{BACKEND_PORT}"
    try:
        import qrcode

        qr = qrcode.QRCode(border=1)
        qr.add_data(url)
        qr.make(fit=True)
        print(f"\n{'=' * 52}")
        print(f"  Scan to reach the backend from your phone:")
        print(f"  {url}")
        print(f"{'=' * 52}\n")
        qr.print_ascii(invert=True)
        print()
    except ImportError:
        print(f"\n[qr] pip install qrcode  for a terminal QR code")
        print(f"[qr] Backend URL: {url}\n")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def main() -> None:
    parser = argparse.ArgumentParser(
        description="KnockKnock one-shot network setup.",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Re-generate certs and patch config.h even if the IP has not changed.",
    )
    args = parser.parse_args()

    print("KnockKnock — network setup\n")

    ip = get_lan_ip()
    print(f"[network] LAN IP: {ip}")

    current_ip = get_config_ip()
    if current_ip:
        print(f"[config]  Current IP in config.h: {current_ip}")

    if current_ip == ip and not args.force:
        print("\n[✓] IP unchanged — skipping cert generation and config patch.")
        print("    Run with --force to regenerate certs anyway.\n")
        print_qr(ip)
        return

    if current_ip is None:
        print("[config]  No existing IP found in config.h — running full setup.\n")
    elif current_ip != ip:
        print(f"[config]  IP changed: {current_ip} → {ip}\n")
    else:
        print(f"[config]  --force: regenerating certs for {ip}\n")

    generate_certs(ip)
    copy_ca_to_firmware()
    patch_config_h(ip)
    print_qr(ip)

    print("Done.")
    print("Next:")
    print("  1. docker compose -f cloud-infrastructure/docker-compose.yml up -d")
    print("  2. cd edge-hub && pio run -t upload")


if __name__ == "__main__":
    main()
