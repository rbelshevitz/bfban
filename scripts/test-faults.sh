#!/bin/sh
set -eu

if [ "${BFBAN_FAKE_MODE:-}" = bfcli ] || [ "${BFBAN_FAKE_MODE:-}" = bfban ]; then
    printf '%s\n' "$*" >> "${BFBAN_FAKE_LOG:?}"
    if [ "${BFBAN_FAKE_MODE}" = bfcli ] &&
       [ "${1:-}" = chain ] && [ "${2:-}" = get ]; then
        exit 1
    fi
    exit 0
fi

build_dir=${1:-build}
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/bfban-test.XXXXXX")
daemon_pid=
trap 'if [ -n "${daemon_pid}" ]; then kill "$daemon_pid" 2>/dev/null || true; wait "$daemon_pid" 2>/dev/null || true; fi; rm -rf "$test_dir"' EXIT

export BFBAN_FAKE_LOG="$test_dir/fake.log"
export BFBAN_BPFCLI=/bin/true
export BFBAN_STATE="$test_dir/banlist"
export BFBAN_FAKE_MODE=bfcli

"$build_dir/bfban" init
"$build_dir/bfban" add 192.0.2.17/24
grep -F '192.0.2.0/24' "$BFBAN_STATE" >/dev/null
"$build_dir/bfban" remove 192.0.2.17/24
test ! -s "$BFBAN_STATE"

printf '%s\n' '198.51.100.1' '198.51.100.2/24' > "$test_dir/feed"
"$build_dir/bfban" add-many "$test_dir/feed"
grep -F '198.51.100.1' "$BFBAN_STATE" >/dev/null
grep -F '198.51.100.0/24' "$BFBAN_STATE" >/dev/null

socket_path="$test_dir/bfban.sock"
daemon_state="$test_dir/daemon.state"
export BFBAN_SOCKET="$socket_path"
export BFBAN_DAEMON_STATE="$daemon_state"
export BFBAN_BIN=/bin/true
export BFBAN_CHAIN=bfban_test
export BFBAN_FAKE_MODE=bfban
"$build_dir/bfband" > "$test_dir/daemon.log" 2>&1 &
daemon_pid=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do
    [ -S "$socket_path" ] && break
    sleep 0.1
done
[ -S "$socket_path" ]

if [ "$(id -u)" -eq 0 ]; then
    "$build_dir/bfbandctl" ping >/dev/null
    "$build_dir/bfbandctl" add 198.51.100.10 >/dev/null
    "$build_dir/bfbandctl" flush >/dev/null
    "$build_dir/bfbandctl" add 198.51.100.11 >/dev/null
    list=$("$build_dir/bfbandctl" list)
    grep -F '198.51.100.11' "$daemon_state" >/dev/null
    test "$(printf '%s\n' "$list" | grep -c '^198.51.100.11 ')" -eq 1
    ! printf '%s\n' "$list" | grep -F '198.51.100.10' >/dev/null

    python3 - "$socket_path" <<'PY'
import socket
import sys
import time

sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.settimeout(4.0)
sock.connect(sys.argv[1])
started = time.monotonic()
data = sock.recv(128)
elapsed = time.monotonic() - started
assert data.startswith(b"ERR "), data
assert elapsed < 3.5, elapsed
sock.close()
PY
else
    echo "bfband protocol tests: skipped (daemon requires root peer)"
fi

echo "bfban fault-path tests: PASS"
