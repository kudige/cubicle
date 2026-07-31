set -eu

tmpdir=$(mktemp -d)
manager_pid=

cleanup() {
    if [ -n "${manager_pid:-}" ]; then
        python3 - "$socket_path" <<'PY' >/dev/null 2>&1 || true
import socket
import sys

client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
client.connect(sys.argv[1])
client.sendall(b"shutdown\n")
client.shutdown(socket.SHUT_WR)
client.recv(1024)
client.close()
PY
        wait "$manager_pid" 2>/dev/null || true
    fi
    rm -rf "$tmpdir"
}

trap cleanup EXIT

state_dir="$tmpdir/manager"
socket_path="$tmpdir/manager.sock"

workspace_output=$("$CUBICLE_MANAGER" --state-dir "$state_dir" workspace create "Project A")
workspace_id=${workspace_output#workspace id=}
workspace_id=${workspace_id%% name=*}

register_output=$("$CUBICLE_MANAGER" --state-dir "$state_dir" process register \
    --workspace "$workspace_id" \
    --friendly-name daemon-1 \
    --mode stream \
    --controller-id controller-1 \
    --control-socket "$tmpdir/controller.sock")

process_id=${register_output#process id=}
process_id=${process_id%% workspace_id=*}

mkdir -p "$state_dir/controllers/$process_id"
cat >"$state_dir/controllers/$process_id/events.log" <<EOF
seq=1 type=process_started controller_id=controller-1 pid=1 pgid=1 mode=stream
seq=2 type=output stream=stdout start=0 length=6
seq=3 type=process_exited status=exited exit_code=0
EOF

python3 - "$socket_path" <<'PY'
import socket
import sys

server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.bind(sys.argv[1])
server.close()
PY

"$CUBICLE_MANAGER" --state-dir "$state_dir" daemon --control-socket "$socket_path" --event-interval-ms 50 &
manager_pid=$!

for _ in $(seq 1 100); do
    if [ -S "$socket_path" ]; then
        break
    fi
    sleep 0.05
done

if [ ! -S "$socket_path" ]; then
    echo "manager daemon did not create control socket" >&2
    exit 1
fi

send_manager_command() {
    python3 - "$socket_path" "$1" <<'PY'
import socket
import sys

client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
client.connect(sys.argv[1])
client.sendall((sys.argv[2] + "\n").encode())
client.shutdown(socket.SHUT_WR)
chunks = []
while True:
    chunk = client.recv(4096)
    if not chunk:
        break
    chunks.append(chunk)
client.close()
sys.stdout.buffer.write(b"".join(chunks))
PY
}

ping_response=$(send_manager_command ping)
case "$ping_response" in
    ok\ manager_id=*\ protocol_major=0\ protocol_minor=1) ;;
    *)
        echo "unexpected manager ping response: $ping_response" >&2
        exit 1
        ;;
esac

manager_id=${ping_response#ok manager_id=}
manager_id=${manager_id%% protocol_major=*}
manager_id=${manager_id% }
grep -q "^$manager_id$" "$state_dir/manager-id"

status_response=$(send_manager_command status)
case "$status_response" in
    ok\ manager_id="$manager_id"\ workspace_count=1\ process_count=1) ;;
    *)
        echo "unexpected manager status response: $status_response" >&2
        exit 1
        ;;
esac

for _ in $(seq 1 100); do
    if [ -f "$state_dir/workspace-events.log" ] &&
        grep -q "^$workspace_id	$process_id	daemon-1	seq=3 type=process_exited status=exited exit_code=0" "$state_dir/workspace-events.log"; then
        break
    fi
    sleep 0.05
done

grep -q "^$workspace_id	$process_id	daemon-1	seq=1 type=process_started" "$state_dir/workspace-events.log"
grep -q "^$workspace_id	$process_id	daemon-1	seq=2 type=output stream=stdout" "$state_dir/workspace-events.log"
grep -q "^$workspace_id	$process_id	daemon-1	seq=3 type=process_exited status=exited exit_code=0" "$state_dir/workspace-events.log"
grep -q "^$process_id	3$" "$state_dir/cursors.tsv"

unknown_response=$(send_manager_command unknown)
if [ "$unknown_response" != "error unsupported" ]; then
    echo "unexpected unknown-command response: $unknown_response" >&2
    exit 1
fi

shutdown_response=$(send_manager_command shutdown)
if [ "$shutdown_response" != "ok" ]; then
    echo "unexpected shutdown response: $shutdown_response" >&2
    exit 1
fi

wait "$manager_pid"
manager_pid=

if [ -S "$socket_path" ]; then
    echo "manager daemon did not remove control socket" >&2
    exit 1
fi
