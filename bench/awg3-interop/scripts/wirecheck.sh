#!/bin/sh
# Prints, in decimal, the little-endian uint32 that sits at the AWG type offset
# (== S1) of the first captured datagram of the given length -- exactly the
# value a receiver feeds to its H1 range check. With header protection on this
# is ciphertext, so it must NOT land inside H1; with it off it must.
#
#   usage: wirecheck.sh <pcap> <S1> <datagram-length>
set -eu
PCAP=$1; S1=$2; PLEN=$3

# tcpdump -x starts its dump at the IP header: 20 bytes IP + 8 bytes UDP.
OFF=$((28 + S1))

tcpdump -r "$PCAP" -n -x 2>/dev/null | awk -v off="$OFF" -v plen="$PLEN" '
function hv(c) { return index("0123456789abcdef", c) - 1 }
/UDP, length/ {
    if (want) { done = 1 }
    want = (!done && $NF + 0 == plen)
    next
}
want && /0x[0-9a-f]+:/ {
    for (i = 2; i <= NF; i++) hex = hex $i
    next
}
END {
    if (length(hex) < 2 * (off + 4)) { print "NOPKT"; exit }
    s = substr(hex, 2 * off + 1, 8)
    # little-endian: byte 0 is the least significant
    v = 0
    for (b = 3; b >= 0; b--) {
        hi = hv(substr(s, 2 * b + 1, 1)); lo = hv(substr(s, 2 * b + 2, 1))
        v = v * 256 + hi * 16 + lo
    }
    # %d clamps at INT_MAX in awk; these are unsigned 32-bit values.
    printf "%.0f\n", v
}'
