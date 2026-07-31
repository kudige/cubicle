set -eu

tmpdir=$(mktemp -d)
trap 'if [ -n "${controller_pid:-}" ]; then kill "$controller_pid" 2>/dev/null || true; wait "$controller_pid" 2>/dev/null || true; fi; rm -rf "$tmpdir"' EXIT

state_dir="$tmpdir/state"
socket_path="$tmpdir/control.sock"
stdout_file="$tmpdir/stdout"
stderr_file="$tmpdir/stderr"
stdin_response="$tmpdir/stdin-response"

"$CUBICLE_CONTROLLER" --state-dir "$state_dir" --control-socket "$socket_path" --mode stream -- sh -c 'IFS= read -r line; printf "got:%s\n" "$line"' >"$stdout_file" 2>"$stderr_file" &
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

python3 - "$socket_path" "$stdin_response" <<'PY'
import socket
import sys

path, output_path = sys.argv[1], sys.argv[2]
client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
client.connect(path)
client.sendall(b"attach stdin\n")
response = b""
while not response.endswith(b"\n"):
    chunk = client.recv(4096)
    if not chunk:
        break
    response += chunk
client.sendall(b"cubicle-input\n")
client.close()

with open(output_path, "wb") as output:
    output.write(response)
PY

set +e
wait "$controller_pid"
status=$?
controller_pid=
set -e

if [ "$status" -ne 0 ]; then
    echo "expected controller status 0, got $status" >&2
    exit 1
fi

printf "ok attached stream=stdin\n" | cmp - "$stdin_response"
printf "got:cubicle-input\n" | cmp - "$state_dir/stdout.log"
grep -q 'type=client_attached stream=stdin$' "$state_dir/events.log"
grep -q 'type=input length=14$' "$state_dir/events.log"
grep -q 'type=client_detached stream=stdin$' "$state_dir/events.log"
