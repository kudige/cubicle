set -eu

tmpdir=$(mktemp -d)
trap 'if [ -S "$tmpdir/control.sock" ]; then python3 "$CUBICLE_CONTROL_CLIENT" "$tmpdir/control.sock" terminate >/dev/null 2>&1 || true; fi; rm -rf "$tmpdir"' EXIT

state_dir="$tmpdir/state"
socket_path="$tmpdir/control.sock"

"$CUBICLE_CONTROLLER" --daemon --state-dir "$state_dir" --control-socket "$socket_path" --mode stream -- sh -c 'printf "daemon-ready\n"; sleep 30'

for _ in $(seq 1 100); do
    if [ -S "$socket_path" ]; then
        break
    fi
    sleep 0.05
done

if [ ! -S "$socket_path" ]; then
    echo "daemon control socket was not created" >&2
    exit 1
fi

status_response=$(python3 "$CUBICLE_CONTROL_CLIENT" "$socket_path" status)
case "$status_response" in
    ok\ state=running\ pid=*\ pgid=*\ stdout_offset=*\ stderr_offset=*) ;;
    *)
        echo "unexpected daemon status response: $status_response" >&2
        exit 1
        ;;
esac

for _ in $(seq 1 100); do
    if [ -f "$state_dir/stdout.log" ] && grep -q 'daemon-ready' "$state_dir/stdout.log"; then
        break
    fi
    sleep 0.05
done

printf "daemon-ready\n" | cmp - "$state_dir/stdout.log"

terminate_response=$(python3 "$CUBICLE_CONTROL_CLIENT" "$socket_path" terminate)
if [ "$terminate_response" != "ok" ]; then
    echo "unexpected daemon terminate response: $terminate_response" >&2
    exit 1
fi

for _ in $(seq 1 100); do
    if [ ! -S "$socket_path" ] && grep -q 'type=process_exited status=signaled signal=15$' "$state_dir/events.log"; then
        break
    fi
    sleep 0.05
done

if [ -S "$socket_path" ]; then
    echo "daemon control socket was not removed after termination" >&2
    exit 1
fi

grep -q 'type=signal_delivered signal=15$' "$state_dir/events.log"
grep -q 'type=process_exited status=signaled signal=15$' "$state_dir/events.log"
