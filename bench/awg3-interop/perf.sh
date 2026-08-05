#!/bin/bash
# Throughput / loss controls, run on the same containers as the interop matrix.
#
# The matrix reported ~2-4 Mbit/s and some ping loss on EVERY protocol version,
# v1 included. Since v1 is the long-standing path that predates all of the v3
# work, a version-independent number is evidence about the sandbox rather than
# about the proxy -- but that has to be measured, not assumed. These three runs
# separate the layers:
#
#   A  plain wireguard-go <-> wireguard-go     no proxy, no AWG   = sandbox cost
#   B  amneziawg-go  <-> amneziawg-go (v3)     no proxy           = + AWG cost
#   C  wireguard-go -> awg-proxy -> amneziawg-go (v3)             = + our cost
set -u
cd "$(dirname "$0")"

SRV=awg3-server; PRX=awg3-proxy; CLI=awg3-client
dk() { MSYS_NO_PATHCONV=1 docker "$@"; }

stop_all() {
    dk exec $CLI pkill -f wireguard-go 2>/dev/null
    dk exec $CLI pkill -f amneziawg-go 2>/dev/null
    dk exec $SRV pkill -f amneziawg-go 2>/dev/null
    dk exec $SRV pkill -f wireguard-go 2>/dev/null
    dk exec $SRV pkill -f "iperf3 -s"  2>/dev/null
    dk exec $PRX pkill -f awg-proxy    2>/dev/null
    sleep 1
}

# measure <label> <server-tunnel-ip> <client-iface>
measure() {
    local label=$1 sip=$2 iface=$3
    dk exec -d $SRV iperf3 -s -B "$sip" >/dev/null 2>&1
    sleep 2

    # Settle first: the proxy rebinds its outbound socket to the client's port
    # on the first transport packet, so a ping started immediately would time a
    # one-off reconnect rather than the steady state.
    dk exec $CLI ping -c 3 -W 3 "$sip" >/dev/null 2>&1
    local ping_out
    ping_out=$(dk exec $CLI ping -c 20 -i 0.3 -W 2 "$sip" 2>&1 | tail -2)

    local ipf
    ipf=$(dk exec $CLI iperf3 -c "$sip" -t 6 -O 2 --connect-timeout 8000 2>&1 \
          | awk '/receiver/ {print $(NF-2), $(NF-1)}')

    printf '  %-22s throughput=%-16s\n' "$label" "${ipf:-FAILED}"
    echo "$ping_out" | sed 's/^/      /'
    dk exec $SRV pkill -f "iperf3 -s" 2>/dev/null
}

echo "=== A. plain WireGuard both ends, no proxy (sandbox floor) ==="
stop_all
dk exec $SRV /scripts/baseline.sh server | sed 's/^/      /'
dk exec $CLI /scripts/baseline.sh client | sed 's/^/      /'
sleep 3
measure "A plain-wg" 10.66.0.1 wgb0

echo
echo "=== B. amneziawg-go v3 direct, no proxy (reference AWG cost) ==="
stop_all
dk exec -e PROFILE=v3 $SRV /scripts/server.sh | sed 's/^/      /'
dk exec -e PROFILE=v3 $CLI /scripts/awg-direct.sh | sed 's/^/      /'
sleep 3
measure "B awg-direct-v3" 10.55.0.1 awgd0

echo
echo "=== C. through our proxy, v3 ==="
stop_all
dk exec -e PROFILE=v3 $SRV /scripts/server.sh | sed 's/^/      /'
dk exec -e PROFILE=v3 $PRX /scripts/proxy.sh  | sed 's/^/      /'
dk exec $CLI /scripts/client.sh > /dev/null
sleep 3
measure "C proxy-v3" 10.55.0.1 wg0

echo
echo "=== D. through our proxy, v2 (header protection off) ==="
stop_all
dk exec -e PROFILE=v2 $SRV /scripts/server.sh | sed 's/^/      /'
dk exec -e PROFILE=v2 $PRX /scripts/proxy.sh  | sed 's/^/      /'
dk exec $CLI /scripts/client.sh > /dev/null
sleep 3
measure "D proxy-v2" 10.55.0.1 wg0
