#!/bin/bash
# Reference measurement: plain wireguard-go on BOTH ends, no proxy, no AWG.
# Same containers, same userspace TUN stack - isolates the environment's own
# cost from anything the proxy adds.
set -eu
. /perf/profile.env
. /shared/keys.env

ROLE=$1          # server | client
IFACE=wgb0
SOCK=/var/run/wireguard/$IFACE.sock
PORT=51830

pkill -f "wireguard-go $IFACE" 2>/dev/null || true
rm -f "$SOCK"; sleep 0.3

LOG_LEVEL=error WG_PROCESS_FOREGROUND=1 wireguard-go -f "$IFACE" >>/shared/baseline.log 2>&1 &
for _ in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.2; done

if [ "$ROLE" = server ]; then
    {
        echo "set=1"
        echo "private_key=$SERVER_PRIV_HEX"
        echo "listen_port=$PORT"
        echo "public_key=$CLIENT_PUB_HEX"
        echo "allowed_ip=10.66.0.2/32"
        echo ""
    } | socat - "UNIX-CONNECT:$SOCK" | head -1
    ip addr add 10.66.0.1/24 dev $IFACE
else
    {
        echo "set=1"
        echo "private_key=$CLIENT_PRIV_HEX"
        echo "listen_port=51831"
        echo "public_key=$SERVER_PUB_HEX"
        echo "endpoint=$SRV_IP:$PORT"
        echo "allowed_ip=10.66.0.0/24"
        echo "persistent_keepalive_interval=15"
        echo ""
    } | socat - "UNIX-CONNECT:$SOCK" | head -1
    ip addr add 10.66.0.2/24 dev $IFACE
fi
ip link set mtu 1420 up dev $IFACE
echo "baseline $ROLE up on $IFACE"
