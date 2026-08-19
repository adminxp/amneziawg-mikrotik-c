#!/bin/bash
# Our awg-proxy in normal mode: plain WG on the client side, AWG upstream.
#
# PROFILE     = v1 | v1.5 | v2 | v3 | v3.1   primary profile
# PRX_HPK     = override the profile's header protection key on this side only
# FB_PROFILE  = optional AWG_FB_* fallback stage (profile name)
# FB_AFTER    = seconds of remote silence before probing the fallback stage
# FAMILY      = v4 (default) | v6 | dual   how AWG_REMOTE names the server
# HE_DELAY    = AWG_HE_DELAY override (only meaningful with FAMILY=dual)
set -eu
. /scripts/profile.env
. /scripts/profiles.sh
. /shared/keys.env

LOG=/shared/proxy.log
PROFILE=${PROFILE:-v3}
FB_PROFILE=${FB_PROFILE:-}

pkill -f awg-proxy 2>/dev/null || true
sleep 0.3

load_profile "$PROFILE"
USE_HPK=${PRX_HPK:-$HPK_ON}

# An IPv6 literal has to be bracketed -- the port is separated by a colon and
# the address is full of them. "dual" uses the container name so the proxy sees
# a real A + AAAA pair and runs the Happy Eyeballs probe.
FAMILY=${FAMILY:-v4}
case "$FAMILY" in
    v4)   REMOTE="$SRV_IP:$SRV_PORT" ;;
    v6)   REMOTE="[$SRV_IP6]:$SRV_PORT" ;;
    dual) REMOTE="$SRV_HOST:$SRV_PORT" ;;
    *)    echo "unknown FAMILY: $FAMILY" >&2; exit 1 ;;
esac

export AWG_LISTEN=":$PRX_PORT"
export AWG_REMOTE="$REMOTE"
[ -n "${HE_DELAY:-}" ] && export AWG_HE_DELAY="$HE_DELAY"
export AWG_MODE=normal
export AWG_JC=$JC AWG_JMIN=$JMIN AWG_JMAX=$JMAX
export AWG_S1=$S1 AWG_S2=$S2 AWG_S3=$S3 AWG_S4=$S4
export AWG_H1=$H1 AWG_H2=$H2 AWG_H3=$H3 AWG_H4=$H4
export AWG_SERVER_PUB=$SERVER_PUB_B64
export AWG_CLIENT_PUB=$CLIENT_PUB_B64
export AWG_LOG_LEVEL=debug
[ -n "$I1" ] && export AWG_I1="$I1"
[ -n "$I2" ] && export AWG_I2="$I2"
[ "$USE_HPK" = 1 ] && export AWG_HEADER_PROTECTION_KEY=$HPK_HEX
[ "$RT_ON" = 1 ] && export AWG_RANDOM_TRAILERS=on
[ "$DC_ON" = 1 ] && export AWG_DISABLE_COOKIES=on

# Optional fallback stage. The key is shared by the whole chain, so a v3->v2
# degradation is expressed by leaving AWG_FB_HP unset (defaults to 0).
if [ -n "$FB_PROFILE" ]; then
    load_profile "$FB_PROFILE"
    export AWG_FB_S1=$S1 AWG_FB_S2=$S2 AWG_FB_S3=$S3 AWG_FB_S4=$S4
    export AWG_FB_H1=$H1 AWG_FB_H2=$H2 AWG_FB_H3=$H3 AWG_FB_H4=$H4
    [ -n "$I1" ] && export AWG_FB_I1="$I1"
    [ -n "$I2" ] && export AWG_FB_I2="$I2"
    [ "$HPK_ON" = 1 ] && export AWG_FB_HP=1
    [ "$RT_ON" = 1 ] && export AWG_FB_RANDOM_TRAILERS=on
    export AWG_FB_AFTER=${FB_AFTER:-10}
fi

awg-proxy >>"$LOG" 2>&1 &
sleep 1

pgrep -f awg-proxy >/dev/null || { echo "FATAL: proxy died"; tail -30 "$LOG"; exit 1; }
echo "proxy up: profile=$PROFILE HPK=$USE_HPK RT=$RT_ON DC=$DC_ON fallback=${FB_PROFILE:-none} remote=$REMOTE"
