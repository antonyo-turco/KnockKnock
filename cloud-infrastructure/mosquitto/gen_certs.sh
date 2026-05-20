#!/usr/bin/env bash
# Usage: ./gen_certs.sh <LAN_IP_OR_HOSTNAME>
# Example: ./gen_certs.sh 192.168.1.42
# The CN must match what CLOUD_MQTT_URI uses in edge-hub/include/config.h.
set -euo pipefail

if [ $# -ne 1 ]; then
    echo "Usage: $0 <LAN_IP_OR_HOSTNAME>"
    exit 1
fi

SERVER_CN="$1"
CERTS_DIR="$(dirname "$0")/certs"
mkdir -p "$CERTS_DIR"

# CA key + self-signed cert
openssl genrsa -out "$CERTS_DIR/ca.key" 2048
openssl req -new -x509 -days 3650 \
    -key "$CERTS_DIR/ca.key" \
    -out "$CERTS_DIR/ca.crt" \
    -subj "/CN=KnockKnock-CA/O=KnockKnock"

# Server key + CSR
openssl genrsa -out "$CERTS_DIR/server.key" 2048
openssl req -new \
    -key "$CERTS_DIR/server.key" \
    -out "$CERTS_DIR/server.csr" \
    -subj "/CN=${SERVER_CN}/O=KnockKnock"

# Sign server cert with CA
openssl x509 -req -days 3650 \
    -in  "$CERTS_DIR/server.csr" \
    -CA  "$CERTS_DIR/ca.crt" \
    -CAkey "$CERTS_DIR/ca.key" \
    -CAcreateserial \
    -out "$CERTS_DIR/server.crt"

echo ""
echo "Certificates written to $CERTS_DIR/"
echo "Next step: copy ca.crt into the firmware tree:"
echo "  cp $CERTS_DIR/ca.crt ../../edge-hub/certs/ca.crt"
echo "Then rebuild and flash the edge-hub firmware."
