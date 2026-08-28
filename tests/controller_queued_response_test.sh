set -eu

tmpdir=$(mktemp -d)
trap 'if [ -n "${controller_pid:-}" ]; then kill "$controller_pid" 2>/dev/null || true; wait "$controller_pid" 2>/dev/null || true; fi; if [ -n "${slow_pid:-}" ]; then kill "$slow_pid" 2>/dev/null || true; wait "$slow_pid" 2>/dev/null || true; fi; rm -rf "$tmpdir"' EXIT

state_dir="$tmpdir/state"
socket_path="$tmpdir/control.sock"

"$CUBICLE_CONTROLLER" \
    --state-dir "$state_dir" \
    --control-socket "$socket_path" \
    --mode stream \
    -- python3 -c 'import sys, time; sys.stdout.buffer.write(b"x" * 65536); sys.stdout.flush(); time.sleep(30)' \
    >/dev/null 2>/dev/null &
controller_pid=$!

for _ in $(seq 1 100); do
    if [ -S "$socket_path" ]; then
        break
    fi
    sleep 0.05
done

if [ ! -S "$socket_path" ]; then
    echo "control socket was not created" >&2
    exit 1
fi

for _ in $(seq 1 100); do
    if [ -f "$state_dir/stdout.log" ] &&
       [ "$(wc -c <"$state_dir/stdout.log")" -eq 65536 ]; then
        break
    fi
    sleep 0.05
done

if [ "$(wc -c <"$state_dir/stdout.log")" -ne 65536 ]; then
    echo "controller did not capture large stdout" >&2
    exit 1
fi

python3 - "$socket_path" "$tmpdir/slow-ready" <<'PY' &
import socket
import sys
import time

path, ready_path = sys.argv[1], sys.argv[2]
client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
client.connect(path)
client.sendall(b"read stdout 0 65536\n")
open(ready_path, "w").close()
time.sleep(1.0)
client.close()
PY
slow_pid=$!

for _ in $(seq 1 100); do
    if [ -f "$tmpdir/slow-ready" ]; then
        break
    fi
    sleep 0.05
done

if [ ! -f "$tmpdir/slow-ready" ]; then
    echo "slow read client did not connect" >&2
    exit 1
fi

status_response=$(python3 "$CUBICLE_CONTROL_CLIENT" "$socket_path" status)
case "$status_response" in
    ok\ state=running\ pid=*\ pgid=*\ stdout_offset=65536\ stderr_offset=0) ;;
    *)
        echo "unexpected status response with queued read client: $status_response" >&2
        exit 1
        ;;
esac

terminate_response=$(python3 "$CUBICLE_CONTROL_CLIENT" "$socket_path" terminate)
if [ "$terminate_response" != "ok" ]; then
    echo "unexpected terminate response with queued read client: $terminate_response" >&2
    exit 1
fi

set +e
wait "$controller_pid"
status=$?
controller_pid=
set -e

if [ "$status" -ne 143 ]; then
    echo "expected terminated controller status 143, got $status" >&2
    exit 1
fi

wait "$slow_pid" 2>/dev/null || true
slow_pid=
