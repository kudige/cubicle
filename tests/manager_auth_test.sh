#!/bin/sh
set -eu

tmpdir=$(mktemp -d)
trap 'if [ -n "${manager_pid:-}" ]; then kill "$manager_pid" 2>/dev/null || true; wait "$manager_pid" 2>/dev/null || true; fi; rm -rf "$tmpdir"' EXIT

state_dir="$tmpdir/state"
runtime_dir="$tmpdir/runtime"
log_dir="$tmpdir/log"
socket_path="$runtime_dir/manager.sock"
key_dir="$tmpdir/client-keys"
mkdir -p "$state_dir" "$runtime_dir" "$log_dir"
chmod 0700 "$state_dir" "$runtime_dir" "$log_dir"

"$CUBICLE_MANAGER" \
    --state-dir "$state_dir" \
    --runtime-dir "$runtime_dir" \
    --log-dir "$log_dir" \
    --controller-bin "$CUBICLE_CONTROLLER" \
    daemon --control-socket "$socket_path" --event-interval-ms 50 \
    >"$tmpdir/manager.out" 2>"$tmpdir/manager.err" &
manager_pid=$!

for _ in $(seq 1 100); do
    if [ -S "$socket_path" ]; then
        break
    fi
    sleep 0.05
done

if [ ! -S "$socket_path" ]; then
    echo "manager socket was not created" >&2
    cat "$tmpdir/manager.err" >&2 || true
    exit 1
fi

"$CUBICLE_MANAGER_AUTH_CLIENT" "$socket_path" "$key_dir"

python3 "$CUBICLE_API_CLIENT" "$socket_path" shutdown >/dev/null
wait "$manager_pid"
manager_pid=
