set -eu

tmpdir=$(mktemp -d)
manager_pid=
running_controller_pid=

cleanup() {
    if [ -n "${manager_pid:-}" ]; then
        python3 "$CUBICLE_API_CLIENT" "$socket_path" shutdown \
            >/dev/null 2>&1 || true
        wait "$manager_pid" 2>/dev/null || true
    fi
    if [ -n "${running_controller_pid:-}" ]; then
        kill "$running_controller_pid" 2>/dev/null || true
        wait "$running_controller_pid" 2>/dev/null || true
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

lost_register_output=$("$CUBICLE_MANAGER" --state-dir "$state_dir" process register \
    --workspace "$workspace_id" \
    --friendly-name daemon-lost \
    --mode stream \
    --controller-id controller-lost \
    --control-socket "$tmpdir/lost-controller.sock")

lost_process_id=${lost_register_output#process id=}
lost_process_id=${lost_process_id%% workspace_id=*}

running_register_output=$("$CUBICLE_MANAGER" --state-dir "$state_dir" process register \
    --workspace "$workspace_id" \
    --friendly-name daemon-running \
    --mode stream \
    --controller-id controller-running \
    --control-socket "$tmpdir/running-controller.sock")

running_process_id=${running_register_output#process id=}
running_process_id=${running_process_id%% workspace_id=*}

mkdir -p "$state_dir/controllers/$process_id"
printf "hello\n" >"$state_dir/controllers/$process_id/stdout.log"
printf "error\n" >"$state_dir/controllers/$process_id/stderr.log"
cat >"$state_dir/controllers/$process_id/events.log" <<EOF
seq=1 type=process_started controller_id=controller-1 pid=1 pgid=1 mode=stream
seq=2 type=output stream=stdout start=0 length=6
seq=3 type=process_exited status=exited exit_code=0
EOF

"$CUBICLE_CONTROLLER" \
    --state-dir "$state_dir/controllers/$running_process_id" \
    --control-socket "$tmpdir/running-controller.sock" \
    --mode stream \
    --stdin-policy open \
    -- sleep 30 &
running_controller_pid=$!

for _ in $(seq 1 100); do
    if [ -S "$tmpdir/running-controller.sock" ]; then
        break
    fi
    sleep 0.05
done

if [ ! -S "$tmpdir/running-controller.sock" ]; then
    echo "running controller did not create control socket" >&2
    exit 1
fi

python3 - "$socket_path" <<'PY'
import socket
import sys

server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.bind(sys.argv[1])
server.close()
PY

"$CUBICLE_MANAGER" --state-dir "$state_dir" daemon --foreground --control-socket "$socket_path" --event-interval-ms 50 &
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

for _ in $(seq 1 100); do
    if grep -q "^$process_id	$workspace_id	daemon-1	stream	completed	" "$state_dir/processes.tsv" &&
        grep -q "^$lost_process_id	$workspace_id	daemon-lost	stream	lost	" "$state_dir/processes.tsv" &&
        grep -q "^$running_process_id	$workspace_id	daemon-running	stream	running	" "$state_dir/processes.tsv"; then
        break
    fi
    sleep 0.05
done

grep -q "^$process_id	$workspace_id	daemon-1	stream	completed	" "$state_dir/processes.tsv"
grep -q "^$lost_process_id	$workspace_id	daemon-lost	stream	lost	" "$state_dir/processes.tsv"
grep -q "^$running_process_id	$workspace_id	daemon-running	stream	running	" "$state_dir/processes.tsv"

send_manager_rpc() {
    python3 "$CUBICLE_API_CLIENT" "$socket_path" call "$@"
}

send_manager_command() {
    case "$1" in
        ping) python3 "$CUBICLE_API_CLIENT" "$socket_path" ping ;;
        status) python3 "$CUBICLE_API_CLIENT" "$socket_path" status ;;
        shutdown) python3 "$CUBICLE_API_CLIENT" "$socket_path" shutdown ;;
        *) send_manager_rpc "$1" ;;
    esac
}

# Endpoint test for manager.ping
ping_response=$(send_manager_command ping)
printf "%s" "$ping_response" | grep -q '"success": true'
printf "%s" "$ping_response" | grep -q '"protocol_major": 0'
printf "%s" "$ping_response" | grep -q '"protocol_minor": 1'
manager_id=$(python3 - "$ping_response" <<'PY'
import json
import sys
print(json.loads(sys.argv[1])["result"]["manager_id"])
PY
)
grep -q "^$manager_id$" "$state_dir/manager-id"

# Endpoint test for manager.status
status_response=$(send_manager_command status)
printf "%s" "$status_response" | grep -q '"success": true'
printf "%s" "$status_response" | grep -q "\"manager_id\": \"$manager_id\""
printf "%s" "$status_response" | grep -q '"workspace_count": 1'
printf "%s" "$status_response" | grep -q '"process_count": 3'

# Endpoint test for manager.reconcile
reconcile_response=$(send_manager_rpc manager.reconcile)
printf "%s" "$reconcile_response" | grep -q '"success": true'

# Endpoint test for workspace.create
workspace_create_response=$(send_manager_rpc workspace.create '{"name":"Project B"}')
printf "%s" "$workspace_create_response" | grep -q '"success": true'
printf "%s" "$workspace_create_response" | grep -q '"name": "Project B"'
workspace_b_id=$(python3 - "$workspace_create_response" <<'PY'
import json
import sys
print(json.loads(sys.argv[1])["result"]["id"])
PY
)

# Endpoint test for workspace.get
workspace_get_response=$(send_manager_rpc workspace.get '{"workspace":"Project B"}')
printf "%s" "$workspace_get_response" | grep -q '"success": true'
printf "%s" "$workspace_get_response" | grep -q "\"id\": \"$workspace_b_id\""

# Endpoint test for workspace.list
workspace_list_response=$(send_manager_rpc workspace.list)
printf "%s" "$workspace_list_response" | grep -q '"count": 2'
printf "%s" "$workspace_list_response" | grep -q '"name": "Project A"'
printf "%s" "$workspace_list_response" | grep -q '"name": "Project B"'

# Endpoint test for workspace.create error response
workspace_duplicate_response=$(python3 "$CUBICLE_API_CLIENT" "$socket_path" \
    --allow-error workspace-create "Project B")
printf "%s" "$workspace_duplicate_response" | grep -q '"success": false'
printf "%s" "$workspace_duplicate_response" | grep -q '"code": "already_exists"'

# Endpoint test for process.get
process_get_response=$(send_manager_rpc process.get "{\"process\":\"$process_id\"}")
printf "%s" "$process_get_response" | grep -q '"success": true'
printf "%s" "$process_get_response" | grep -q "\"id\": \"$process_id\""
printf "%s" "$process_get_response" | grep -q '"friendly_name": "daemon-1"'
printf "%s" "$process_get_response" | grep -q '"state": "completed"'

# Endpoint test for process.get workspace-local name lookup
process_name_response=$(send_manager_rpc process.get "{\"process\":\"daemon-1\",\"workspace_id\":\"$workspace_id\"}")
printf "%s" "$process_name_response" | grep -q '"success": true'
printf "%s" "$process_name_response" | grep -q "\"id\": \"$process_id\""

# Endpoint test for process.list
process_list_response=$(send_manager_rpc process.list "{\"workspace_id\":\"$workspace_id\"}")
printf "%s" "$process_list_response" | grep -q '"count": 3'
printf "%s" "$process_list_response" | grep -q '"friendly_name": "daemon-1"'
printf "%s" "$process_list_response" | grep -q '"friendly_name": "daemon-lost"'
printf "%s" "$process_list_response" | grep -q '"state": "lost"'
printf "%s" "$process_list_response" | grep -q '"friendly_name": "daemon-running"'
printf "%s" "$process_list_response" | grep -q '"state": "running"'

# Endpoint test for process.read_output
read_output_response=$(send_manager_rpc process.read_output "{\"process_id\":\"$process_id\",\"stream\":\"stdout\",\"offset\":0,\"maximum_length\":16}")
printf "%s" "$read_output_response" | grep -q '"success": true'
printf "%s" "$read_output_response" | grep -q '"start_offset": 0'
printf "%s" "$read_output_response" | grep -q '"next_offset": 6'
printf "%s" "$read_output_response" | grep -q '"end_of_stream": true'
printf "%s" "$read_output_response" | grep -q '"data": "hello\\n"'

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
grep -Eq "^$process_id	3	[1-9][0-9]*$" "$state_dir/cursors.tsv"

# Endpoint test for events.list
events_list_response=$(send_manager_rpc events.list "{\"workspace_id\":\"$workspace_id\",\"process_id\":\"$process_id\",\"after_sequence\":0,\"limit\":10}")
printf "%s" "$events_list_response" | grep -q '"success": true'
printf "%s" "$events_list_response" | grep -q '"count": 3'
printf "%s" "$events_list_response" | grep -q '"type": "process_started"'
printf "%s" "$events_list_response" | grep -q '"type": "output_available"'
printf "%s" "$events_list_response" | grep -q '"type": "process_exited"'

# Endpoint test for manager.cleanup
cleanup_response=$(send_manager_rpc manager.cleanup "{\"workspace_id\":\"$workspace_id\"}")
printf "%s" "$cleanup_response" | grep -q '"success": true'
printf "%s" "$cleanup_response" | grep -q '"removed_count": 2'
printf "%s" "$cleanup_response" | grep -q '"skipped_live_count": 1'
process_list_after_cleanup=$(send_manager_rpc process.list "{\"workspace_id\":\"$workspace_id\"}")
printf "%s" "$process_list_after_cleanup" | grep -q '"count": 1'
printf "%s" "$process_list_after_cleanup" | grep -q '"friendly_name": "daemon-running"'

# Endpoint test for unsupported endpoint error response
unknown_response=$(python3 "$CUBICLE_API_CLIENT" "$socket_path" \
    --allow-error call unknown)
if ! printf "%s" "$unknown_response" | grep -q '"code": "unsupported"'; then
    echo "unexpected unknown-command response: $unknown_response" >&2
    exit 1
fi

# Endpoint test for manager.shutdown
shutdown_response=$(send_manager_command shutdown)
if ! printf "%s" "$shutdown_response" | grep -q '"success": true'; then
    echo "unexpected shutdown response: $shutdown_response" >&2
    exit 1
fi

wait "$manager_pid"
manager_pid=

if [ -S "$socket_path" ]; then
    echo "manager daemon did not remove control socket" >&2
    exit 1
fi

detached_state_dir="$tmpdir/detached-manager"
detached_socket_path="$tmpdir/detached-manager.sock"
if ! "$CUBICLE_MANAGER" --state-dir "$detached_state_dir" daemon \
    --control-socket "$detached_socket_path" --event-interval-ms 50 \
    >"$tmpdir/detached-manager.out" 2>"$tmpdir/detached-manager.err"; then
    echo "detached manager daemon startup failed" >&2
    cat "$tmpdir/detached-manager.err" >&2
    exit 1
fi
grep -q '^\[INFO\] manager: ' "$tmpdir/detached-manager.err"

for _ in $(seq 1 100); do
    if [ -S "$detached_socket_path" ]; then
        break
    fi
    sleep 0.05
done

if [ ! -S "$detached_socket_path" ]; then
    echo "detached manager daemon did not keep running" >&2
    exit 1
fi

detached_ping=$(python3 "$CUBICLE_API_CLIENT" "$detached_socket_path" ping)
printf "%s" "$detached_ping" | grep -q '"success": true'
python3 "$CUBICLE_API_CLIENT" "$detached_socket_path" shutdown >/dev/null
for _ in $(seq 1 100); do
    if [ ! -S "$detached_socket_path" ]; then
        break
    fi
    sleep 0.05
done
if [ -S "$detached_socket_path" ]; then
    echo "detached manager daemon did not remove control socket" >&2
    exit 1
fi
