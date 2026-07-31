set -eu

tmpdir=$(mktemp -d)
trap 'if [ -n "${controller_pid:-}" ]; then kill "$controller_pid" 2>/dev/null || true; wait "$controller_pid" 2>/dev/null || true; fi; rm -rf "$tmpdir"' EXIT

state_dir="$tmpdir/state"
socket_path="$tmpdir/control.sock"

"$CUBICLE_CONTROLLER" --state-dir "$state_dir" --control-socket "$socket_path" --mode stream -- sh -c 'printf "recovery\n"; sleep 1' >/dev/null 2>/dev/null &
controller_pid=$!

for _ in $(seq 1 100); do
    if [ -S "$socket_path" ] && [ -f "$state_dir/events.log" ] && grep -q 'type=output stream=stdout' "$state_dir/events.log"; then
        break
    fi
    sleep 0.05
done

if [ ! -S "$socket_path" ]; then
    echo "control socket was not created" >&2
    exit 1
fi

python3 "$CUBICLE_CONTROL_CLIENT" "$socket_path" metadata >"$tmpdir/metadata-response"
grep -q '^ok length=' "$tmpdir/metadata-response"
grep -q '^controller_id=' "$tmpdir/metadata-response"
grep -q '^mode=stream$' "$tmpdir/metadata-response"

mode_response=$(python3 "$CUBICLE_CONTROL_CLIENT" "$socket_path" mode)
if [ "$mode_response" != "stream" ]; then
    echo "unexpected mode response: $mode_response" >&2
    exit 1
fi

python3 "$CUBICLE_CONTROL_CLIENT" "$socket_path" events-after 0 10 >"$tmpdir/events-response"
grep -q '^ok count=.* length=' "$tmpdir/events-response"
grep -q '^seq=1 type=process_started' "$tmpdir/events-response"
grep -q 'type=output stream=stdout' "$tmpdir/events-response"

python3 "$CUBICLE_CONTROL_CLIENT" "$socket_path" events-after 1 10 >"$tmpdir/events-after-response"
grep -q '^ok count=.* length=' "$tmpdir/events-after-response"
grep -q '^seq=2 type=output stream=stdout' "$tmpdir/events-after-response"

set +e
wait "$controller_pid"
status=$?
controller_pid=
set -e

if [ "$status" -ne 0 ]; then
    echo "expected controller status 0, got $status" >&2
    exit 1
fi
