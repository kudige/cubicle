set -eu

tmpdir=$(mktemp -d)
trap 'if [ -n "${controller_pid:-}" ]; then kill "$controller_pid" 2>/dev/null || true; wait "$controller_pid" 2>/dev/null || true; fi; rm -rf "$tmpdir"' EXIT

eof_state="$tmpdir/eof-state"

"$CUBICLE_CONTROLLER" --stdin-policy eof --state-dir "$eof_state" --mode stream -- cat >/dev/null 2>/dev/null

grep -q '^stdin_policy=eof$' "$eof_state/metadata"
[ ! -s "$eof_state/stdout.log" ]
grep -q 'type=process_exited status=exited exit_code=0$' "$eof_state/events.log"

state_dir="$tmpdir/state"
socket_path="$tmpdir/control.sock"

"$CUBICLE_CONTROLLER" --stdin-policy eof --state-dir "$state_dir" --control-socket "$socket_path" --mode stream -- sh -c 'sleep 1' >/dev/null 2>/dev/null &
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

response=$(python3 "$CUBICLE_CONTROL_CLIENT" "$socket_path" raw "attach stdin")
if [ "$response" != "error stdin_closed" ]; then
    echo "unexpected attach stdin response: $response" >&2
    exit 1
fi

set +e
wait "$controller_pid"
status=$?
controller_pid=
set -e

if [ "$status" -ne 0 ]; then
    echo "expected controller status 0, got $status" >&2
    exit 1
fi

grep -q '^stdin_policy=eof$' "$state_dir/metadata"
