#!/bin/bash
# Control measurement: amneziawg-go client talking DIRECTLY to the amneziawg-go
# server on the same profile - no proxy in the path. Tells us whether a slow
# transfer is the proxy's doing or the cost of userspace AWG in this sandbox.
#
# PROFILE = v1 | v1.5 | v2 | v3   (must match whatever the server is running)
set -eu
. /scripts/profile.env
. /scripts/profiles.sh
. /shared/keys.env

PROFILE=${PROFILE:-v3}
load_profile "$PROFILE"

IFACE=awgd0
SOCK=/var/run/amneziawg/$IFACE.sock

pkill -f "amneziawg-go $IFACE" 2>/dev/null || true
rm -f "$SOCK"; sleep 0.3

LOG_LEVEL=error WG_PROCESS_FOREGROUND=1 amneziawg-go -f "$IFACE" >>/shared/awgdirect.log 2>&1 &
for _ in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.2; done
[ -S "$SOCK" ] || { echo "FATAL: no UAPI socket"; exit 1; }

{
    echo "set=1"
    echo "private_key=$CLIENT_PRIV_HEX"
    echo "listen_port=51841"
    echo "jc=$JC"; echo "jmin=$JMIN"; echo "jmax=$JMAX"
    echo "s1=$S1"; echo "s2=$S2"; echo "s3=$S3"; echo "s4=$S4"
    echo "h1=$H1"; echo "h2=$H2"; echo "h3=$H3"; echo "h4=$H4"
    [ -n "$I1" ] && echo "i1=$I1"
    [ -n "$I2" ] && echo "i2=$I2"
    [ "$HPK_ON" = 1 ] && echo "header_protection_key=$HPK_HEX"
    echo "public_key=$SERVER_PUB_HEX"
    echo "endpoint=$SRV_IP:$SRV_PORT"
    echo "allowed_ip=10.55.0.0/$TUN_MASK"
    echo "persistent_keepalive_interval=15"
    echo ""
} | socat - "UNIX-CONNECT:$SOCK" | head -1

ip addr add "$CLI_TUN/$TUN_MASK" dev $IFACE 2>/dev/null || true
ip link set mtu 1420 up dev $IFACE
echo "awg-direct client up on $IFACE ($PROFILE) -> $SRV_IP:$SRV_PORT"
