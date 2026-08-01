set -eu

tmpdir=$(mktemp -d)
manager_pid=

cleanup() {
    if [ -n "${manager_pid:-}" ]; then
        python3 "$CUBICLE_API_CLIENT" "$socket_path" shutdown \
            >/dev/null 2>&1 || true
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
    --friendly-name aggregate-error \
    --mode stream \
    --controller-id controller-1 \
    --control-socket "$tmpdir/controller.sock")
process_id=${register_output#process id=}
process_id=${process_id%% workspace_id=*}

mkdir -p "$state_dir/controllers/$process_id"
cat >"$state_dir/controllers/$process_id/events.log" <<EOF
seq=1 type=process_started controller_id=controller-1 pid=1 pgid=1 mode=stream
EOF
mkdir "$state_dir/workspace-events.log"

"$CUBICLE_MANAGER" --state-dir "$state_dir" daemon \
    --control-socket "$socket_path" --event-interval-ms 50 \
    2>"$tmpdir/manager.err" &
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
    if grep -q "open $state_dir/workspace-events.log:" "$tmpdir/manager.err"; then
        break
    fi
    sleep 0.05
done

grep -q "open $state_dir/workspace-events.log:" "$tmpdir/manager.err"

ping_response=$(python3 "$CUBICLE_API_CLIENT" "$socket_path" ping)
printf "%s" "$ping_response" | grep -q '"success": true'

shutdown_response=$(python3 "$CUBICLE_API_CLIENT" "$socket_path" shutdown)
printf "%s" "$shutdown_response" | grep -q '"success": true'
wait "$manager_pid"
manager_pid=
