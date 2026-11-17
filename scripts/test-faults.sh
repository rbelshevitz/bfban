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
backend_log="$test_dir/backend.log"
export BFBAN_SOCKET="$socket_path"
export BFBAN_DAEMON_STATE="$daemon_state"
export BFBAN_BIN=/bin/true
export BFBAN_CHAIN=bfban_test
export BFBAN_TEST_BACKEND_LOG="$backend_log"

start_daemon() {
    rm -f "$socket_path"
    "$build_dir/bfband" > "$test_dir/daemon.log" 2>&1 &
    daemon_pid=$!
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        [ -S "$socket_path" ] && return 0
        sleep 0.1
    done
    cat "$test_dir/daemon.log" >&2
    return 1
}

stop_daemon() {
    if [ -n "$daemon_pid" ]; then
        kill "$daemon_pid" 2>/dev/null || true
        wait "$daemon_pid" 2>/dev/null || true
        daemon_pid=
    fi
}

start_daemon

if [ "$(id -u)" -eq 0 ]; then
    ctl="$build_dir/bfbandctl"
    "$ctl" ping >/dev/null
    "$ctl" add 198.51.100.10 >/dev/null
    "$ctl" flush >/dev/null
    "$ctl" add 198.51.100.11 >/dev/null
    list=$("$ctl" list)
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

    stop_daemon
    rm -f "$daemon_state" "$backend_log"
    export BFBAN_TEST_FAIL_SAVE_STAGE=before_rename
    start_daemon
    if "$ctl" add 198.51.100.20 >/dev/null 2>&1; then
        echo "expected save failure was accepted" >&2
        exit 1
    fi
    grep -F 'add 198.51.100.20' "$backend_log" >/dev/null
    grep -F 'remove 198.51.100.20' "$backend_log" >/dev/null
    "$ctl" ping >/dev/null

    stop_daemon
    rm -f "$daemon_state" "$backend_log"
    export BFBAN_TEST_FAIL_BACKEND_COMMAND=remove
    export BFBAN_TEST_FAIL_BACKEND_AT=1
    start_daemon
    if "$ctl" add 198.51.100.21 >/dev/null 2>&1; then
        echo "expected rollback failure was accepted" >&2
        exit 1
    fi
    if "$ctl" add 198.51.100.22 >/dev/null 2>&1; then
        echo "reconciliation mode did not block mutation" >&2
        exit 1
    fi
    "$ctl" init >/dev/null
    unset BFBAN_TEST_FAIL_BACKEND_COMMAND BFBAN_TEST_FAIL_BACKEND_AT

    stop_daemon
    rm -f "$daemon_state" "$backend_log"
    export BFBAN_TEST_FAIL_SAVE_STAGE=
    export BFBAN_TEST_FAIL_BACKEND_COMMAND=add
    export BFBAN_TEST_FAIL_BACKEND_AT=2
    start_daemon
    printf '%s\n' '203.0.113.1' '203.0.113.2' > "$test_dir/batch-feed"
    if "$ctl" add-many "$test_dir/batch-feed" >/dev/null 2>&1; then
        echo "expected partial batch failure was accepted" >&2
        exit 1
    fi
    test ! -s "$daemon_state"
    test "$(grep -c '^remove ' "$backend_log")" -eq 2
    unset BFBAN_TEST_FAIL_BACKEND_COMMAND BFBAN_TEST_FAIL_BACKEND_AT

    stop_daemon
    rm -f "$daemon_state" "$backend_log"
    export BFBAN_TEST_FAIL_SAVE_STAGE=after_rename
    start_daemon
    "$ctl" add 203.0.113.10 >/dev/null
    grep -F '203.0.113.10' "$daemon_state" >/dev/null
    unset BFBAN_TEST_FAIL_SAVE_STAGE
    stop_daemon
    echo "bfband fault-injection tests: PASS"
else
    stop_daemon
    echo "bfband protocol/fault-injection tests: SKIP (daemon requires root peer)"
fi

echo "bfban basic tests: PASS"
