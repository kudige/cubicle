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
xdg_state_home="$tmpdir/xdg-state"

workspace_output=$("$CUBICLE_MANAGER" --state-dir "$state_dir" workspace create "Project A")
workspace_id=${workspace_output#workspace id=}
workspace_id=${workspace_id%% name=*}

"$CUBICLE_MANAGER" --state-dir "$state_dir" process register \
    --workspace "$workspace_id" \
    --friendly-name build \
    --mode stream \
    --controller-id controller-1 \
    --control-socket "$tmpdir/controller.sock" \
    --process-id process-1 >/dev/null

"$CUBICLE_MANAGER" --state-dir "$state_dir" daemon \
    --control-socket "$socket_path" --event-interval-ms 50 &
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

cube() {
    XDG_STATE_HOME="$xdg_state_home" \
        CUBICLE_MANAGER_SOCKET="$socket_path" \
        "$CUBE" "$@"
}

ps_output=$(cube --workspace "Project A" ps)
printf "%s\n" "$ps_output" | grep -q '^Workspace Project A$'
printf "%s\n" "$ps_output" | grep -q '^NAME	MODE	STATE$'
printf "%s\n" "$ps_output" | grep -q '^build	stream	running$'

json_ps_output=$(cube --workspace "Project A" --json ps)
printf "%s" "$json_ps_output" | grep -q '"processes"'
printf "%s" "$json_ps_output" | grep -q '"friendly_name":"build"'
printf "%s" "$json_ps_output" | grep -q '"count":1'

inspect_output=$(cube --workspace "Project A" inspect build)
printf "%s\n" "$inspect_output" | grep -q '^Name:        build$'
printf "%s\n" "$inspect_output" | grep -q '^Workspace:   Project A$'
printf "%s\n" "$inspect_output" | grep -q '^Mode:        stream$'
printf "%s\n" "$inspect_output" | grep -q '^State:       running$'
printf "%s\n" "$inspect_output" | grep -q '^Process ID:  process-1$'

json_inspect_output=$(cube --workspace "Project A" --json inspect build)
printf "%s" "$json_inspect_output" | grep -q '"id":"process-1"'
printf "%s" "$json_inspect_output" | grep -q '"friendly_name":"build"'

cube workspace select "Project A" >/dev/null
selected_ps_output=$(cube ps)
printf "%s\n" "$selected_ps_output" | grep -q '^build	stream	running$'

set +e
XDG_STATE_HOME="$tmpdir/empty-state" CUBICLE_MANAGER_SOCKET="$socket_path" \
    "$CUBE" ps >"$tmpdir/no-workspace.out" 2>"$tmpdir/no-workspace.err"
status=$?
set -e
if [ "$status" -ne 1 ]; then
    echo "cube ps without selected workspace should exit 1, got $status" >&2
    exit 1
fi
grep -q 'no workspace selected' "$tmpdir/no-workspace.err"

set +e
cube inspect >"$tmpdir/inspect-missing.out" 2>"$tmpdir/inspect-missing.err"
status=$?
set -e
if [ "$status" -ne 2 ]; then
    echo "cube inspect without process should exit 2, got $status" >&2
    exit 1
fi
grep -q 'inspect requires a process name' "$tmpdir/inspect-missing.err"

shutdown_response=$(python3 "$CUBICLE_API_CLIENT" "$socket_path" shutdown)
printf "%s" "$shutdown_response" | grep -q '"success": true'
wait "$manager_pid"
manager_pid=
