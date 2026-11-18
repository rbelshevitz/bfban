#!/bin/sh
set -eu

usage() {
    echo "Usage: sudo $0 INTERFACE" >&2
    echo "Example: sudo $0 eno1" >&2
}

if [ "$#" -ne 1 ]; then
    usage
    exit 2
fi

IFACE=$1
IFACE_PATH="/sys/class/net/$IFACE"

if [ ! -d "$IFACE_PATH" ]; then
    echo "check-xdp: interface not found: $IFACE" >&2
    exit 1
fi

if [ "$(id -u)" -ne 0 ]; then
    echo "check-xdp: run as root so bpftool can probe the device" >&2
    exit 1
fi

command -v ip >/dev/null 2>&1 || {
    echo "check-xdp: iproute2 is required" >&2
    exit 1
}
command -v bpftool >/dev/null 2>&1 || {
    echo "check-xdp: bpftool is required" >&2
    exit 1
}

IFINDEX=$(cat "$IFACE_PATH/ifindex")

echo "Interface: $IFACE"
echo "ifindex:   $IFINDEX"
echo
echo "Link details:"
ip -details link show dev "$IFACE"

echo
echo "Driver details:"
if command -v ethtool >/dev/null 2>&1; then
    ethtool -i "$IFACE"
else
    echo "ethtool is not installed; driver details unavailable" >&2
fi

echo
echo "Kernel eBPF probe:"
bpftool feature probe kernel | grep -E 'eBPF|JIT|BPF_PROG_TYPE_XDP|BPF_MAP_TYPE' || true

echo
echo "Device eBPF probe:"
bpftool feature probe dev "$IFACE"

echo
echo "Current BPF network attachments:"
bpftool net show dev "$IFACE" || true

cat <<EOF

Preflight complete for $IFACE (ifindex $IFINDEX).
This check does not attach an XDP program and cannot prove native XDP support.
Test the actual bfban chain on a disposable interface, then verify it with:
  bpftool net show dev $IFACE
  ip -details link show dev $IFACE
EOF
