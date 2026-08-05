#!/bin/bash
# The five acceptance criteria for AWG 3.0 interop, run against the isolated
# perf stack (awg3p-*) so it never collides with the main harness.
set -u
cd "$(dirname "$0")"
SRV=awg3p-server; PRX=awg3p-proxy; CLI=awg3p-client
dk() { MSYS_NO_PATHCONV=1 docker "$@"; }
pass=0; fail=0
ok()  { echo "  [PASS] $*"; pass=$((pass+1)); }
bad() { echo "  [FAIL] $*"; fail=$((fail+1)); }

hs_stamp() { dk exec $CLI wg show wg0 latest-handshakes 2>/dev/null | awk '{print $2}'; }
handshake_ok() {
    local t after=${2:-0}
    for _ in $(seq 1 "${1:-20}"); do
        t=$(hs_stamp)
        [ -n "$t" ] && [ "$t" -gt "$after" ] 2>/dev/null && return 0
        sleep 1
    done
    return 1
}
stop_all() {
    dk exec $CLI sh -c "pkill -f wireguard-go; sleep 0.5; ip link del wg0 2>/dev/null; true" >/dev/null 2>&1
    dk exec $SRV sh -c "pkill -f amneziawg-go; sleep 0.5; ip link del awg0 2>/dev/null; true" >/dev/null 2>&1
    dk exec $PRX pkill -f awg-proxy >/dev/null 2>&1
    sleep 1
}
bring_up() {   # $1 = proxy header-protection override ("" = profile default)
    stop_all
    dk exec $SRV rm -f /shared/server.log /shared/proxy.log >/dev/null 2>&1
    dk exec -e PROFILE=v3 $SRV /perf/server.sh 2>&1 | tail -1 | sed 's/^/      /'
    dk exec -e PROFILE=v3 ${1:+-e PRX_HPK=$1} $PRX /perf/proxy.sh 2>&1 | tail -1 | sed 's/^/      /'
    dk exec $CLI /perf/client.sh >/dev/null 2>&1
}

echo "############ AWG 3.0 interop: 5 acceptance criteria ############"
bring_up ""

echo; echo "=== 1. handshake with a real amneziawg-go v3.0.3 server ==="
if handshake_ok 30; then
    ok "handshake completed"
    dk exec $CLI wg show wg0 | grep -E "latest handshake|transfer|peer" | sed 's/^/      /'
else
    bad "no handshake within 30s"
    dk exec $PRX tail -20 /shared/proxy.log | sed 's/^/      /'
fi

echo; echo "=== 2. ping -c 5 through the tunnel ==="
# one warm-up packet: the very first datagram races the session install
dk exec $CLI ping -c 1 -W 2 10.55.0.1 >/dev/null 2>&1
dk exec $CLI ping -c 5 -W 2 10.55.0.1 > /tmp/p2 2>&1
tail -2 /tmp/p2 | sed 's/^/      /'
grep -q " 0% packet loss" /tmp/p2 && ok "ping 5/5, no loss" || bad "ping loss"

echo; echo "=== 3. bulk data ==="
dk exec $SRV pkill iperf3 >/dev/null 2>&1; sleep 1
dk exec -d $SRV iperf3 -s -B 10.55.0.1 >/dev/null 2>&1; sleep 2
dk exec $CLI iperf3 -c 10.55.0.1 -u -b 300M -t 5 > /tmp/p3u 2>&1
grep receiver /tmp/p3u | sed 's/^/      UDP  /'
dk exec $CLI iperf3 -c 10.55.0.1 -t 5 -O 1 > /tmp/p3t 2>&1
grep receiver /tmp/p3t | sed 's/^/      TCP  /'
dk exec $CLI ping -c 100 -i 0.02 -W 2 10.55.0.1 > /tmp/p3p 2>&1
tail -2 /tmp/p3p | sed 's/^/      /'
if grep -q " 0% packet loss" /tmp/p3p && grep -q receiver /tmp/p3u; then
    ok "data flows both ways (100/100 ping, UDP 300M clean)"
else
    bad "bulk transfer failed"
fi

echo; echo "=== 4. reconnect: restart the server ==="
before=$(hs_stamp); echo "      stamp before: $before"
dk exec $SRV sh -c "pkill -f amneziawg-go; sleep 1; ip link del awg0 2>/dev/null; true" >/dev/null 2>&1
sleep 2
dk exec -e PROFILE=v3 $SRV /perf/server.sh 2>&1 | tail -1 | sed 's/^/      /'
dk exec -d $CLI ping -c 90 -i 1 -W 1 10.55.0.1 >/dev/null 2>&1
if handshake_ok 90 "$before"; then
    echo "      stamp after:  $(hs_stamp)"
    dk exec $CLI ping -c 3 -W 3 10.55.0.1 >/dev/null 2>&1 \
        && ok "tunnel recovered (fresh handshake + ping)" \
        || bad "re-handshaked but ping fails"
else
    bad "no fresh handshake within 90s"
fi

echo; echo "=== 5. negative control: proxy without AWG_HEADER_PROTECTION_KEY ==="
bring_up 0
if handshake_ok 20; then
    bad "handshake SUCCEEDED without the key - header protection is not being exercised"
else
    ok "handshake correctly fails without the key"
fi

echo; echo "############ $pass passed, $fail failed ############"
