#!/bin/bash
# Plain WireGuard client (wireguard-go, no AWG awareness at all).
# Its endpoint is the proxy; everything AWG-specific happens downstream.
set -eu
. /scripts/profile.env
. /shared/keys.env

IFACE=wg0
SOCK=/var/run/wireguard/$IFACE.sock
LOG=/shared/client.log

pkill -f "wireguard-go $IFACE" 2>/dev/null || true
rm -f "$SOCK"
sleep 0.3

LOG_LEVEL=verbose WG_PROCESS_FOREGROUND=1 \
    wireguard-go -f "$IFACE" >>"$LOG" 2>&1 &

for _ in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.2; done
[ -S "$SOCK" ] || { echo "FATAL: wg UAPI socket never appeared"; tail -20 "$LOG"; exit 1; }

{
    echo "set=1"
    echo "private_key=$CLIENT_PRIV_HEX"
    echo "listen_port=$CLI_PORT"
    echo "public_key=$SERVER_PUB_HEX"
    echo "endpoint=$PRX_IP:$PRX_PORT"
    echo "allowed_ip=10.55.0.0/$TUN_MASK"
    echo "persistent_keepalive_interval=15"
    echo ""
} > /tmp/uapi.txt

RESP=$(socat - "UNIX-CONNECT:$SOCK" < /tmp/uapi.txt)
echo "UAPI response: $RESP"
echo "$RESP" | grep -q "errno=0" || { echo "FATAL: UAPI set failed"; exit 1; }

ip addr add "$CLI_TUN/$TUN_MASK" dev "$IFACE"
# Over an IPv6 upstream the outer datagram grows by 20 bytes, so the caller
# lowers this to keep a full-size packet inside the 1500-byte underlay.
ip link set mtu "${WG_MTU:-1420}" up dev "$IFACE"

echo "client up: $IFACE $CLI_TUN -> endpoint $PRX_IP:$PRX_PORT mtu=${WG_MTU:-1420}"
