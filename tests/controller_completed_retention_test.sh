set -eu

tmpdir=$(mktemp -d)
trap 'if [ -n "${controller_pid:-}" ]; then kill "$controller_pid" 2>/dev/null || true; wait "$controller_pid" 2>/dev/null || true; fi; rm -rf "$tmpdir"' EXIT

state_dir="$tmpdir/state"
socket_path="$tmpdir/control.sock"

"$CUBICLE_CONTROLLER" \
    --completed-retention-ms 2000 \
    --state-dir "$state_dir" \
    --control-socket "$socket_path" \
    --mode stream \
    -- sh -c 'printf "retained\n"' \
    >/dev/null 2>/dev/null &
controller_pid=$!

for _ in $(seq 1 100); do
    if [ -f "$state_dir/events.log" ] &&
       grep -q 'type=process_exited status=exited exit_code=0$' "$state_dir/events.log"; then
        break
    fi
    sleep 0.05
done

if ! grep -q 'type=process_exited status=exited exit_code=0$' "$state_dir/events.log"; then
    echo "controller did not enter completed retention" >&2
    exit 1
fi

status_response=$(python3 "$CUBICLE_CONTROL_CLIENT" "$socket_path" status)
case "$status_response" in
    ok\ state=completed\ pid=*\ pgid=*\ result=0\ stdout_offset=9\ stderr_offset=0) ;;
    *)
        echo "unexpected completed status response: $status_response" >&2
        exit 1
        ;;
esac

read_response=$(python3 "$CUBICLE_CONTROL_CLIENT" "$socket_path" read stdout 0 9)
if [ "$read_response" != "$(printf 'ok length=9\nretained\n')" ]; then
    echo "unexpected completed read response: $read_response" >&2
    exit 1
fi

terminate_response=$(python3 "$CUBICLE_CONTROL_CLIENT" "$socket_path" terminate)
if [ "$terminate_response" != "error process_completed" ]; then
    echo "unexpected completed terminate response: $terminate_response" >&2
    exit 1
fi

set +e
wait "$controller_pid"
status=$?
controller_pid=
set -e

if [ "$status" -ne 0 ]; then
    echo "expected retained controller status 0, got $status" >&2
    exit 1
fi

if [ -S "$socket_path" ]; then
    echo "completed retention socket was not removed" >&2
    exit 1
fi
