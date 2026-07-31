set -eu

tmpdir=$(mktemp -d)
trap 'if [ -n "${controller_pid:-}" ]; then kill "$controller_pid" 2>/dev/null || true; wait "$controller_pid" 2>/dev/null || true; fi; rm -rf "$tmpdir"' EXIT

state_dir="$tmpdir/state"
socket_path="$tmpdir/control.sock"
stdout_file="$tmpdir/stdout"
stderr_file="$tmpdir/stderr"
attach_file="$tmpdir/attach"

"$CUBICLE_CONTROLLER" --state-dir "$state_dir" --control-socket "$socket_path" --mode stream -- sh -c 'printf "first\n"; sleep 0.4; printf "second\n"; sleep 0.4; printf "third\n"' >"$stdout_file" 2>"$stderr_file" &
controller_pid=$!

for _ in $(seq 1 100); do
    if [ -S "$socket_path" ] && [ -f "$state_dir/stdout.log" ] && grep -q 'first' "$state_dir/stdout.log"; then
        break
    fi
    sleep 0.05
done

if ! grep -q 'first' "$state_dir/stdout.log"; then
    echo "controller did not capture initial stdout" >&2
    exit 1
fi

python3 - "$socket_path" "$attach_file" <<'PY' &
import socket
import sys

path, output_path = sys.argv[1], sys.argv[2]
client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
client.connect(path)
client.sendall(b"attach stdout 0\n")
with open(output_path, "wb") as output:
    while True:
        chunk = client.recv(4096)
        if not chunk:
            break
        output.write(chunk)
client.close()
PY
attach_pid=$!

set +e
wait "$controller_pid"
status=$?
controller_pid=
set -e

if [ "$status" -ne 0 ]; then
    echo "expected controller status 0, got $status" >&2
    exit 1
fi

wait "$attach_pid"

grep -q '^ok attached stream=stdout start=0 length=' "$attach_file"
grep -q 'first' "$attach_file"
grep -q 'second' "$attach_file"
grep -q 'third' "$attach_file"

printf "first\nsecond\nthird\n" | cmp - "$state_dir/stdout.log"
grep -q 'type=client_attached stream=stdout$' "$state_dir/events.log"
grep -q 'type=client_detached stream=stdout$' "$state_dir/events.log"
