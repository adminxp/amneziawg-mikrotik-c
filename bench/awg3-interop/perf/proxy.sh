#!/bin/bash
# Our awg-proxy in normal mode: plain WG on the client side, AWG upstream.
#
# PROFILE     = v1 | v1.5 | v2 | v3   primary profile
# PRX_HPK     = override the profile's header protection key on this side only
# FB_PROFILE  = optional AWG_FB_* fallback stage (profile name)
# FB_AFTER    = seconds of remote silence before probing the fallback stage
set -eu
. /perf/profile.env
. /perf/profiles.sh
. /shared/keys.env

LOG=/shared/proxy.log
PROFILE=${PROFILE:-v3}
FB_PROFILE=${FB_PROFILE:-}

pkill -f awg-proxy 2>/dev/null || true
sleep 0.3

load_profile "$PROFILE"
USE_HPK=${PRX_HPK:-$HPK_ON}

export AWG_LISTEN=":$PRX_PORT"
export AWG_REMOTE="$SRV_IP:$SRV_PORT"
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

# Optional fallback stage. The key is shared by the whole chain, so a v3->v2
# degradation is expressed by leaving AWG_FB_HP unset (defaults to 0).
if [ -n "$FB_PROFILE" ]; then
    load_profile "$FB_PROFILE"
    export AWG_FB_S1=$S1 AWG_FB_S2=$S2 AWG_FB_S3=$S3 AWG_FB_S4=$S4
    export AWG_FB_H1=$H1 AWG_FB_H2=$H2 AWG_FB_H3=$H3 AWG_FB_H4=$H4
    [ -n "$I1" ] && export AWG_FB_I1="$I1"
    [ -n "$I2" ] && export AWG_FB_I2="$I2"
    [ "$HPK_ON" = 1 ] && export AWG_FB_HP=1
    export AWG_FB_AFTER=${FB_AFTER:-10}
fi

awg-proxy >>"$LOG" 2>&1 &
sleep 1

pgrep -f awg-proxy >/dev/null || { echo "FATAL: proxy died"; tail -30 "$LOG"; exit 1; }
echo "proxy up: profile=$PROFILE HPK=$USE_HPK fallback=${FB_PROFILE:-none}"
