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

"$CUBICLE_MANAGER" --state-dir "$state_dir" \
    --controller-bin "$CUBICLE_CONTROLLER" \
    daemon --control-socket "$socket_path" --event-interval-ms 50 &
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

cube run --stream --name fg-run sh -c \
    'printf "fg-out\n"; printf "fg-err\n" >&2' \
    >"$tmpdir/fg-run.out" 2>"$tmpdir/fg-run.err"
grep -q '^fg-out$' "$tmpdir/fg-run.out"
grep -q '^fg-err$' "$tmpdir/fg-run.err"
output=$(cube remove fg-run)
if [ "$output" != "Process fg-run removed" ]; then
    echo "unexpected foreground process remove output: $output" >&2
    exit 1
fi

output=$(cube run --bg --stream --name bg-run sleep 30)
if [ "$output" != "[bg-run] started in stream mode" ]; then
    echo "unexpected background run output: $output" >&2
    exit 1
fi
json_bg_output=$(cube --json run --bg --stream /bin/sleep 30)
printf "%s" "$json_bg_output" | grep -q '"friendly_name":"sleep"'
printf "%s" "$json_bg_output" | grep -q '"mode":"stream"'
json_bg_suffix_output=$(cube --json run --bg --stream /bin/sleep 30)
printf "%s" "$json_bg_suffix_output" | grep -q '"friendly_name":"sleep-1"'
printf "%s" "$json_bg_suffix_output" | grep -q '"mode":"stream"'
tty_output=$(cube run --bg --tty --name tty-run sleep 30)
if [ "$tty_output" != "[tty-run] started in tty mode" ]; then
    echo "unexpected tty run output: $tty_output" >&2
    exit 1
fi
cube stop bg-run >/dev/null
cube stop sleep >/dev/null
cube stop sleep-1 >/dev/null
cube stop tty-run >/dev/null

api() {
    python3 "$CUBICLE_API_CLIENT" --raw "$socket_path" "$@"
}

json_id() {
    python3 -c 'import json, sys; print(json.load(sys.stdin)["result"]["id"])'
}

signal_process_id=$(api process-start --workspace "$workspace_id" \
    --friendly-name signal-me sleep 30 | json_id)
output=$(cube signal signal-me TERM)
if [ "$output" != "Process signal-me signaled" ]; then
    echo "unexpected process signal output: $output" >&2
    exit 1
fi
api process-wait "$signal_process_id" --timeout-ms 2000 | grep -q '"success":true'

stop_process_id=$(api process-start --workspace "$workspace_id" \
    --friendly-name stop-me sleep 30 | json_id)
output=$(cube stop stop-me)
if [ "$output" != "Process stop-me stopped" ]; then
    echo "unexpected process stop output: $output" >&2
    exit 1
fi
api process-wait "$stop_process_id" --timeout-ms 2000 | grep -q '"success":true'

kill_process_id=$(api process-start --workspace "$workspace_id" \
    --friendly-name kill-me sleep 30 | json_id)
json_kill_output=$(cube --json kill kill-me)
if [ "$json_kill_output" != "{}" ]; then
    echo "unexpected process kill JSON output: $json_kill_output" >&2
    exit 1
fi
api process-wait "$kill_process_id" --timeout-ms 2000 | grep -q '"success":true'

remove_process_id=$(api process-start --workspace "$workspace_id" \
    --friendly-name remove-me /bin/true | json_id)
api process-wait "$remove_process_id" --timeout-ms 2000 | grep -q '"success":true'
output=$(cube remove remove-me)
if [ "$output" != "Process remove-me removed" ]; then
    echo "unexpected process remove output: $output" >&2
    exit 1
fi
set +e
cube inspect remove-me >"$tmpdir/removed-inspect.out" 2>"$tmpdir/removed-inspect.err"
status=$?
set -e
if [ "$status" -ne 1 ]; then
    echo "removed process inspect should exit 1, got $status" >&2
    exit 1
fi
grep -q 'process not found' "$tmpdir/removed-inspect.err"

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

set +e
cube signal build NOPE >"$tmpdir/signal-invalid.out" 2>"$tmpdir/signal-invalid.err"
status=$?
set -e
if [ "$status" -ne 2 ]; then
    echo "cube signal with invalid signal should exit 2, got $status" >&2
    exit 1
fi
grep -q 'invalid signal' "$tmpdir/signal-invalid.err"

set +e
cube run --term true >"$tmpdir/run-term.out" 2>"$tmpdir/run-term.err"
status=$?
set -e
if [ "$status" -ne 2 ]; then
    echo "cube run --term should exit 2, got $status" >&2
    exit 1
fi
grep -q 'term mode is not implemented yet' "$tmpdir/run-term.err"

set +e
timeout 2 env XDG_STATE_HOME="$xdg_state_home" \
    CUBICLE_MANAGER_SOCKET="$socket_path" \
    "$CUBE" run --tty sleep 30 \
    >"$tmpdir/run-tty-fg.out" 2>"$tmpdir/run-tty-fg.err"
status=$?
set -e
if [ "$status" -ne 2 ]; then
    echo "cube foreground tty run should exit 2, got $status" >&2
    exit 1
fi
grep -q 'foreground tty attach is not implemented yet' "$tmpdir/run-tty-fg.err"
cube ps >"$tmpdir/after-tty-fg-ps.out"
grep -q '^Workspace Project A$' "$tmpdir/after-tty-fg-ps.out"

set +e
cube run >"$tmpdir/run-missing.out" 2>"$tmpdir/run-missing.err"
status=$?
set -e
if [ "$status" -ne 2 ]; then
    echo "cube run without command should exit 2, got $status" >&2
    exit 1
fi
grep -q 'run requires a command' "$tmpdir/run-missing.err"

shutdown_response=$(python3 "$CUBICLE_API_CLIENT" "$socket_path" shutdown)
printf "%s" "$shutdown_response" | grep -q '"success": true'
wait "$manager_pid"
manager_pid=
