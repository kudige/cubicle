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

output=$(cube workspace create "Project A")
if [ "$output" != "Workspace Project A created and selected" ]; then
    echo "unexpected workspace create output: $output" >&2
    exit 1
fi

output=$(cube workspace)
if [ "$output" != "Workspace Project A selected" ]; then
    echo "unexpected selected workspace output: $output" >&2
    exit 1
fi

output=$(cube workspace "Project A")
if [ "$output" != "Workspace Project A selected" ]; then
    echo "unexpected workspace select output: $output" >&2
    exit 1
fi

output=$("$CUBE" --manager-socket "$socket_path" workspace "Project B")
if [ "$output" != "Workspace Project B created and selected" ]; then
    echo "unexpected explicit-socket workspace output: $output" >&2
    exit 1
fi

list_output=$(cube workspace list)
printf "%s\n" "$list_output" | grep -q '^WORKSPACE ID	NAME$'
printf "%s\n" "$list_output" | grep -q '	Project A$'
printf "%s\n" "$list_output" | grep -q '	Project B$'

output=$(cube workspace stop "Project B")
if [ "$output" != "Workspace Project B stopped" ]; then
    echo "unexpected workspace stop output: $output" >&2
    exit 1
fi

output=$(cube workspace delete "Project B")
if [ "$output" != "Workspace Project B deleted" ]; then
    echo "unexpected workspace delete output: $output" >&2
    exit 1
fi

set +e
cube workspace delete "Project B" >"$tmpdir/delete-missing.out" 2>"$tmpdir/delete-missing.err"
status=$?
set -e
if [ "$status" -ne 1 ]; then
    echo "missing workspace delete should exit 1, got $status" >&2
    exit 1
fi
grep -q 'workspace not found' "$tmpdir/delete-missing.err"

list_output=$(cube workspace list)
if printf "%s\n" "$list_output" | grep -q '	Project B$'; then
    echo "deleted workspace was still listed" >&2
    exit 1
fi

shutdown_response=$(python3 "$CUBICLE_API_CLIENT" "$socket_path" shutdown)
printf "%s" "$shutdown_response" | grep -q '"success": true'
wait "$manager_pid"
manager_pid=
