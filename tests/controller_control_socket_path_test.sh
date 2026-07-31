set -eu

tmpdir=$(mktemp -d)
trap 'if [ -n "${controller_pid:-}" ]; then kill "$controller_pid" 2>/dev/null || true; wait "$controller_pid" 2>/dev/null || true; fi; if [ -n "${server_pid:-}" ]; then kill "$server_pid" 2>/dev/null || true; wait "$server_pid" 2>/dev/null || true; fi; rm -rf "$tmpdir"' EXIT

stale_socket="$tmpdir/stale.sock"
python3 - "$stale_socket" <<'PY'
import socket
import sys

path = sys.argv[1]
server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.bind(path)
server.close()
PY

state_dir="$tmpdir/stale-state"
"$CUBICLE_CONTROLLER" \
    --state-dir "$state_dir" \
    --control-socket "$stale_socket" \
    --mode stream \
    -- sh -c 'sleep 30' \
    >/dev/null 2>/dev/null &
controller_pid=$!

stale_status=""
for _ in $(seq 1 100); do
    if [ -S "$stale_socket" ]; then
        stale_status=$(python3 "$CUBICLE_CONTROL_CLIENT" "$stale_socket" status 2>/dev/null || true)
    fi
    case "$stale_status" in
        ok\ state=running\ pid=*\ pgid=*\ stdout_offset=0\ stderr_offset=0)
        break
        ;;
    esac
    sleep 0.05
done

case "$stale_status" in
    ok\ state=running\ pid=*\ pgid=*\ stdout_offset=0\ stderr_offset=0) ;;
    *)
    echo "controller did not replace stale socket: $stale_status" >&2
    exit 1
    ;;
esac

terminate_response=$(python3 "$CUBICLE_CONTROL_CLIENT" "$stale_socket" terminate)
if [ "$terminate_response" != "ok" ]; then
    echo "unexpected stale-socket terminate response: $terminate_response" >&2
    exit 1
fi

set +e
wait "$controller_pid"
status=$?
controller_pid=
set -e

if [ "$status" -ne 143 ]; then
    echo "expected stale-socket controller status 143, got $status" >&2
    exit 1
fi

live_socket="$tmpdir/live.sock"
python3 - "$live_socket" "$tmpdir/live-ready" <<'PY' &
import socket
import sys
import time

path, ready_path = sys.argv[1], sys.argv[2]
server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.bind(path)
server.listen(1)
open(ready_path, "w").close()
time.sleep(30)
server.close()
PY
server_pid=$!

for _ in $(seq 1 100); do
    if [ -f "$tmpdir/live-ready" ]; then
        break
    fi
    sleep 0.05
done

if [ ! -f "$tmpdir/live-ready" ]; then
    echo "live socket server did not start" >&2
    exit 1
fi

set +e
"$CUBICLE_CONTROLLER" \
    --state-dir "$tmpdir/live-state" \
    --control-socket "$live_socket" \
    --mode stream \
    -- sh -c 'sleep 30' \
    >/dev/null 2>"$tmpdir/live-stderr"
status=$?
set -e

if [ "$status" -ne 1 ]; then
    echo "expected controller status 1 for live socket collision, got $status" >&2
    exit 1
fi

grep -q 'failed to initialize control socket:' "$tmpdir/live-stderr"
if [ ! -S "$live_socket" ]; then
    echo "live socket was removed" >&2
    exit 1
fi

kill "$server_pid"
wait "$server_pid" 2>/dev/null || true
server_pid=

file_socket="$tmpdir/file.sock"
printf "not-a-socket\n" >"$file_socket"

set +e
"$CUBICLE_CONTROLLER" \
    --state-dir "$tmpdir/file-state" \
    --control-socket "$file_socket" \
    --mode stream \
    -- sh -c 'sleep 30' \
    >/dev/null 2>"$tmpdir/file-stderr"
status=$?
set -e

if [ "$status" -ne 1 ]; then
    echo "expected controller status 1 for non-socket path, got $status" >&2
    exit 1
fi

grep -q 'failed to initialize control socket:' "$tmpdir/file-stderr"
printf "not-a-socket\n" | cmp - "$file_socket"
