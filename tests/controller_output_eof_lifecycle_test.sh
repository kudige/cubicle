set -eu

tmpdir=$(mktemp -d)
trap 'if [ -n "${controller_pid:-}" ]; then kill "$controller_pid" 2>/dev/null || true; wait "$controller_pid" 2>/dev/null || true; fi; rm -rf "$tmpdir"' EXIT

state_dir="$tmpdir/state"
socket_path="$tmpdir/control.sock"

"$CUBICLE_CONTROLLER" \
    --state-dir "$state_dir" \
    --control-socket "$socket_path" \
    --mode stream \
    -- sh -c 'exec >/dev/null 2>/dev/null; sleep 30' \
    >/dev/null 2>/dev/null &
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

for _ in $(seq 1 100); do
    if [ -f "$state_dir/stdout.log" ] &&
       [ -f "$state_dir/stderr.log" ] &&
       [ ! -s "$state_dir/stdout.log" ] &&
       [ ! -s "$state_dir/stderr.log" ]; then
        break
    fi
    sleep 0.05
done

status_response=$(python3 "$CUBICLE_CONTROL_CLIENT" "$socket_path" status)
case "$status_response" in
    ok\ state=running\ pid=*\ pgid=*\ stdout_offset=0\ stderr_offset=0) ;;
    *)
        echo "unexpected status response after output EOF: $status_response" >&2
        exit 1
        ;;
esac

terminate_response=$(python3 "$CUBICLE_CONTROL_CLIENT" "$socket_path" terminate)
if [ "$terminate_response" != "ok" ]; then
    echo "unexpected terminate response after output EOF: $terminate_response" >&2
    exit 1
fi

set +e
wait "$controller_pid"
status=$?
controller_pid=
set -e

if [ "$status" -ne 143 ]; then
    echo "expected terminated controller status 143, got $status" >&2
    exit 1
fi

grep -q 'type=signal_delivered signal=15$' "$state_dir/events.log"
grep -q 'type=process_exited status=signaled signal=15$' "$state_dir/events.log"
