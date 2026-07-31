set -eu

tmpdir=$(mktemp -d)
controller_pid=
trap 'if [ -n "${controller_pid:-}" ]; then kill "$controller_pid" 2>/dev/null || true; wait "$controller_pid" 2>/dev/null || true; fi; rm -rf "$tmpdir"' EXIT

state_dir="$tmpdir/state"
socket_path="$tmpdir/control.sock"

"$CUBICLE_CONTROLLER" \
    --state-dir "$state_dir" \
    --control-socket "$socket_path" \
    --mode tty \
    --stdin-policy open \
    -- sh -c 'printf "ready\n"; IFS= read -r line; printf "size:%s\n" "$(stty size)"; printf "typed:%s\n" "$line"' \
    >/dev/null 2>"$tmpdir/stderr" &
controller_pid=$!

for _ in $(seq 1 100); do
    if [ -S "$socket_path" ] && [ -f "$state_dir/stdout.log" ] &&
        grep -q 'ready' "$state_dir/stdout.log"; then
        break
    fi
    sleep 0.05
done

if [ ! -S "$socket_path" ]; then
    echo "control socket was not created" >&2
    exit 1
fi

resize_response=$(python3 "$CUBICLE_CONTROL_CLIENT" "$socket_path" resize 40 120)
if [ "$resize_response" != "ok" ]; then
    echo "unexpected resize response: $resize_response" >&2
    exit 1
fi

printf 'hello from socket tty\n' | \
    python3 "$CUBICLE_CONTROL_CLIENT" "$socket_path" attach stdin >"$tmpdir/stdin-response"

set +e
wait "$controller_pid"
status=$?
controller_pid=
set -e

if [ "$status" -ne 0 ]; then
    echo "expected controller status 0, got $status" >&2
    cat "$tmpdir/stderr" >&2
    exit 1
fi

printf "ok attached stream=stdin\n" | cmp - "$tmpdir/stdin-response"
grep -q 'size:40 120' "$state_dir/stdout.log"
grep -q 'typed:hello from socket tty' "$state_dir/stdout.log"
grep -q 'type=terminal_resized rows=40 columns=120$' "$state_dir/events.log"
grep -q 'type=input length=22$' "$state_dir/events.log"
grep -q 'type=client_attached stream=stdin$' "$state_dir/events.log"
grep -q 'type=client_detached stream=stdin$' "$state_dir/events.log"
