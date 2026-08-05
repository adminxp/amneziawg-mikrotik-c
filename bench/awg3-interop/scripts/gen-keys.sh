#!/bin/bash
# Generates both X25519 keypairs and the header protection key.
# base64 form goes to our proxy, hex form goes to UAPI.
set -eu
OUT=${1:-/shared/keys.env}

b2h() { printf '%s' "$1" | base64 -d | xxd -p -c 32; }

SERVER_PRIV_B64=$(wg genkey)
SERVER_PUB_B64=$(printf '%s' "$SERVER_PRIV_B64" | wg pubkey)
CLIENT_PRIV_B64=$(wg genkey)
CLIENT_PUB_B64=$(printf '%s' "$CLIENT_PRIV_B64" | wg pubkey)
HPK_HEX=$(head -c 32 /dev/urandom | xxd -p -c 32)
HPK_B64=$(printf '%s' "$HPK_HEX" | xxd -r -p | base64 -w0)

cat > "$OUT" <<EOF
SERVER_PRIV_B64=$SERVER_PRIV_B64
SERVER_PUB_B64=$SERVER_PUB_B64
SERVER_PRIV_HEX=$(b2h "$SERVER_PRIV_B64")
SERVER_PUB_HEX=$(b2h "$SERVER_PUB_B64")
CLIENT_PRIV_B64=$CLIENT_PRIV_B64
CLIENT_PUB_B64=$CLIENT_PUB_B64
CLIENT_PRIV_HEX=$(b2h "$CLIENT_PRIV_B64")
CLIENT_PUB_HEX=$(b2h "$CLIENT_PUB_B64")
HPK_HEX=$HPK_HEX
HPK_B64=$HPK_B64
EOF
echo "keys written to $OUT"
cat "$OUT"
