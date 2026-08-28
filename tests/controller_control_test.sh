set -eu

tmpdir=$(mktemp -d)
trap 'if [ -n "${controller_pid:-}" ]; then kill "$controller_pid" 2>/dev/null || true; wait "$controller_pid" 2>/dev/null || true; fi; rm -rf "$tmpdir"' EXIT

state_dir="$tmpdir/state"
socket_path="$tmpdir/control.sock"
stdout_file="$tmpdir/stdout"
stderr_file="$tmpdir/stderr"

"$CUBICLE_CONTROLLER" --state-dir "$state_dir" --control-socket "$socket_path" --mode stream -- sh -c 'printf "ready\n"; sleep 30' >"$stdout_file" 2>"$stderr_file" &
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

send_command() {
    python3 - "$socket_path" "$1" <<'PY'
import socket
import sys

path, command = sys.argv[1], sys.argv[2]
client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
client.connect(path)
client.sendall(command.encode("utf-8") + b"\n")
while True:
    chunk = client.recv(4096)
    if not chunk:
        break
    sys.stdout.buffer.write(chunk)
client.close()
PY
}

status_response=$(send_command status)
case "$status_response" in
    ok\ state=running\ pid=*\ pgid=*\ stdout_offset=*\ stderr_offset=*) ;;
    *)
        echo "unexpected status response: $status_response" >&2
        exit 1
        ;;
esac

for _ in $(seq 1 100); do
    read_response=$(send_command "read stdout 0 6")
    if [ "$read_response" = "$(printf 'ok length=6\nready\n')" ]; then
        break
    fi
    sleep 0.05
done

if [ "$read_response" != "$(printf 'ok length=6\nready\n')" ]; then
    echo "unexpected read response: $read_response" >&2
    exit 1
fi

python3 - "$socket_path" "$tmpdir/idle-ready" <<'PY' &
import socket
import sys
import time

path, ready_path = sys.argv[1], sys.argv[2]
client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
client.connect(path)
client.sendall(b"sta")
open(ready_path, "w").close()
time.sleep(1.0)
client.close()
PY
idle_pid=$!

for _ in $(seq 1 100); do
    if [ -f "$tmpdir/idle-ready" ]; then
        break
    fi
    sleep 0.05
done

if [ ! -f "$tmpdir/idle-ready" ]; then
    echo "idle client did not connect" >&2
    exit 1
fi

status_while_idle=$(send_command status)
case "$status_while_idle" in
    ok\ state=running\ pid=*\ pgid=*\ stdout_offset=*\ stderr_offset=*) ;;
    *)
        echo "unexpected status response while idle client was connected: $status_while_idle" >&2
        exit 1
        ;;
esac

wait "$idle_pid"

crlf_response=$(python3 - "$socket_path" <<'PY'
import socket
import sys

path = sys.argv[1]
client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
client.connect(path)
client.sendall(b"status\r\n")
while True:
    chunk = client.recv(4096)
    if not chunk:
        break
    sys.stdout.buffer.write(chunk)
client.close()
PY
)
case "$crlf_response" in
    ok\ state=running\ pid=*\ pgid=*\ stdout_offset=*\ stderr_offset=*) ;;
    *)
        echo "unexpected CRLF status response: $crlf_response" >&2
        exit 1
        ;;
esac

nul_response=$(python3 - "$socket_path" <<'PY'
import socket
import sys

path = sys.argv[1]
client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
client.connect(path)
client.sendall(b"status\0\n")
while True:
    chunk = client.recv(4096)
    if not chunk:
        break
    sys.stdout.buffer.write(chunk)
client.close()
PY
)
if [ "$nul_response" != "error bad_request" ]; then
    echo "unexpected NUL request response: $nul_response" >&2
    exit 1
fi

oversized_response=$(python3 - "$socket_path" <<'PY'
import socket
import sys

path = sys.argv[1]
client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
client.connect(path)
client.sendall(b"x" * 300 + b"\n")
try:
    while True:
        chunk = client.recv(4096)
        if not chunk:
            break
        sys.stdout.buffer.write(chunk)
except ConnectionResetError:
    pass
client.close()
PY
)
if [ "$oversized_response" != "error request_too_long" ]; then
    echo "unexpected oversized request response: $oversized_response" >&2
    exit 1
fi

bad_attach_response=$(send_command "attach stdout")
if [ "$bad_attach_response" != "error bad_attach_command" ]; then
    echo "unexpected bad attach response: $bad_attach_response" >&2
    exit 1
fi

status_after_bad_clients=$(send_command status)
case "$status_after_bad_clients" in
    ok\ state=running\ pid=*\ pgid=*\ stdout_offset=*\ stderr_offset=*) ;;
    *)
        echo "unexpected status response after bad clients: $status_after_bad_clients" >&2
        exit 1
        ;;
esac

terminate_response=$(send_command terminate)
if [ "$terminate_response" != "ok" ]; then
    echo "unexpected terminate response: $terminate_response" >&2
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

printf "ready\n" | cmp - "$state_dir/stdout.log"
grep -q 'type=signal_delivered signal=15$' "$state_dir/events.log"
grep -q 'type=process_exited status=signaled signal=15$' "$state_dir/events.log"
