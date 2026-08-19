#!/bin/bash
# AmneziaWG 3.x server: userspace amneziawg-go + UAPI configuration.
#
# PROFILE = v1 | v1.5 | v2 | v3 | v3.1   (see profiles.sh)
# SRV_HPK = override the profile's header protection key on this side only;
#           used by the negative controls.
set -eu
. /scripts/profile.env
. /scripts/profiles.sh
. /shared/keys.env

IFACE=awg0
SOCK=/var/run/amneziawg/$IFACE.sock
LOG=/shared/server.log

PROFILE=${PROFILE:-v3}
load_profile "$PROFILE"
USE_HPK=${SRV_HPK:-$HPK_ON}

# SRV_IMPL=legacy runs the real pre-3.0 server (v0.2.19) instead of v3.0.3, so
# v1/v1.5/v2 can be checked against a separate implementation rather than a
# reconfigured 3.0. It has no header protection at all.
SRV_IMPL=${SRV_IMPL:-current}
if [ "$SRV_IMPL" = legacy ]; then
    BIN=amneziawg-go-legacy
    [ "$USE_HPK" = 1 ] && { echo "FATAL: legacy server has no header protection"; exit 1; }
else
    BIN=amneziawg-go
fi

pkill -f "amneziawg-go" 2>/dev/null || true
rm -f "$SOCK"
sleep 0.3

LOG_LEVEL=verbose WG_PROCESS_FOREGROUND=1 \
    "$BIN" -f "$IFACE" >>"$LOG" 2>&1 &

for _ in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.2; done
[ -S "$SOCK" ] || { echo "FATAL: UAPI socket never appeared"; tail -20 "$LOG"; exit 1; }

# --- UAPI set. Key names taken verbatim from device/uapi.go (v3.0.3). ---
{
    echo "set=1"
    echo "private_key=$SERVER_PRIV_HEX"
    echo "listen_port=$SRV_PORT"
    echo "jc=$JC"
    echo "jmin=$JMIN"
    echo "jmax=$JMAX"
    echo "s1=$S1"
    echo "s2=$S2"
    echo "s3=$S3"
    echo "s4=$S4"
    echo "h1=$H1"
    echo "h2=$H2"
    echo "h3=$H3"
    echo "h4=$H4"
    [ -n "$I1" ] && echo "i1=$I1"
    [ -n "$I2" ] && echo "i2=$I2"
    [ "$USE_HPK" = 1 ] && echo "header_protection_key=$HPK_HEX"
    [ "$RT_ON" = 1 ] && echo "random_trailers=1"
    [ "$DC_ON" = 1 ] && echo "disable_cookies=1"
    echo "public_key=$CLIENT_PUB_HEX"
    echo "allowed_ip=$CLI_TUN/32"
    echo ""
} > /tmp/uapi.txt

RESP=$(socat - "UNIX-CONNECT:$SOCK" < /tmp/uapi.txt)
echo "$RESP" | grep -q "errno=0" || { echo "FATAL: UAPI set failed: $RESP"; exit 1; }

# Read the state back instead of trusting the log: mergeWithDevice() prints
# "UAPI: Updating header protection key" unconditionally (uapi.go:843), so that
# line says nothing about whether a key is actually in effect. get=1 emits the
# key only when it is non-zero (uapi.go:153), so this is the authoritative check.
STATE=$(printf 'get=1\n\n' | socat - "UNIX-CONNECT:$SOCK")
if echo "$STATE" | grep -q "^header_protection_key="; then ACTUAL=1; else ACTUAL=0; fi
[ "$ACTUAL" = "$USE_HPK" ] || {
    echo "FATAL: header protection mismatch: wanted $USE_HPK, device reports $ACTUAL"
    exit 1
}

# get=1 reports the 3.1 switches unconditionally (uapi.go boolf), so a mismatch
# here means the server build predates 3.1 rather than that the key was ignored.
if [ "$RT_ON" = 1 ]; then
    echo "$STATE" | grep -q "^random_trailers=1" || {
        echo "FATAL: server does not report random_trailers=1 (needs amneziawg-go 3.1)"
        exit 1
    }
fi

ip addr add "$SRV_TUN/$TUN_MASK" dev "$IFACE" 2>/dev/null || true
ip link set mtu 1420 up dev "$IFACE"

echo "server up: impl=$SRV_IMPL profile=$PROFILE S=$S1/$S2/$S3/$S4 HPK=$ACTUAL RT=$RT_ON DC=$DC_ON (verified via get=1)"
