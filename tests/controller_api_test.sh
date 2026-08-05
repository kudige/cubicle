set -eu

tmpdir=$(mktemp -d)
trap 'if [ -n "${controller_pid:-}" ]; then kill "$controller_pid" 2>/dev/null || true; wait "$controller_pid" 2>/dev/null || true; fi; rm -rf "$tmpdir"' EXIT

state_dir="$tmpdir/state"
socket_path="$tmpdir/control.sock"

"$CUBICLE_CONTROLLER" --state-dir "$state_dir" --control-socket "$socket_path" --mode stream -- sh -c 'printf "ready\n"; printf "\377\033[31mX\n"; cat; sleep 30' >/dev/null 2>/dev/null &
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

api() {
    python3 "$CUBICLE_API_CLIENT" --raw "$socket_path" "$@"
}

json_field() {
    python3 - "$1" "$2" <<'PY'
import json
import sys

document = json.loads(sys.argv[1])
value = document
for part in sys.argv[2].split("."):
    value = value[part]
print(value)
PY
}

status_response=$(api controller-status)
if [ "$(json_field "$status_response" success)" != "True" ]; then
    echo "controller.status failed: $status_response" >&2
    exit 1
fi
if [ "$(json_field "$status_response" result.state)" != "running" ]; then
    echo "unexpected controller state: $status_response" >&2
    exit 1
fi

python3 - "$socket_path" <<'PY'
import json
import socket
import struct
import sys

socket_path = sys.argv[1]


def read_exact(sock, size):
    data = b""
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise RuntimeError("short persistent controller response")
        data += chunk
    return data


def call(sock, request_id, method, params):
    payload = json.dumps({
        "protocol_major": 0,
        "protocol_minor": 1,
        "request_id": request_id,
        "session_id": "local-session",
        "method": method,
        "params": params,
    }, separators=(",", ":")).encode("utf-8")
    sock.sendall(struct.pack("!I", len(payload)) + payload)
    header = read_exact(sock, 4)
    length = struct.unpack("!I", header)[0]
    return json.loads(read_exact(sock, length).decode("utf-8"))


with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
    sock.connect(socket_path)
    first = call(sock, "persist-1", "controller.status", {})
    second = call(sock, "persist-2", "controller.read", {
        "stream": "stdout",
        "offset": 0,
        "maximum_length": 6,
    })

if not first.get("success") or first.get("result", {}).get("state") != "running":
    raise SystemExit(f"persistent controller.status failed: {first}")
if not second.get("success") or "data" not in second.get("result", {}):
    raise SystemExit(f"persistent controller.read failed: {second}")
PY

unsupported_response=$(api --allow-error call controller.no_such_method)
if [ "$(json_field "$unsupported_response" success)" != "False" ]; then
    echo "unsupported controller method unexpectedly succeeded: $unsupported_response" >&2
    exit 1
fi

for _ in $(seq 1 100); do
    read_response=$(api controller-read stdout --offset 0 --max 6)
    if [ "$(json_field "$read_response" result.data)" = "ready" ]; then
        break
    fi
    sleep 0.05
done

if [ "$(json_field "$read_response" result.data)" != "ready" ]; then
    echo "unexpected initial read response: $read_response" >&2
    exit 1
fi

binary_response=$(api controller-read stdout --offset 6 --max 8)
if [ "$(json_field "$binary_response" success)" != "True" ]; then
    echo "controller.read failed on binary/control output: $binary_response" >&2
    exit 1
fi
python3 - "$socket_path" <<'PY'
import json
import socket
import struct
import sys

socket_path = sys.argv[1]
def call(sock, request_id, data):
    payload = json.dumps({
        "protocol_major": 0,
        "protocol_minor": 1,
        "request_id": request_id,
        "session_id": "local-session",
        "method": "controller.write",
        "params": {"data": data},
    }, separators=(",", ":")).encode("utf-8")
    sock.sendall(struct.pack("!I", len(payload)) + payload)
    header = sock.recv(4)
    length = struct.unpack("!I", header)[0]
    response = json.loads(sock.recv(length).decode("utf-8"))
    if not response.get("success"):
        raise SystemExit(f"controller.write failed: {response}")

with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
    sock.connect(socket_path)
    call(sock, "utf8-write-prefix", "a" * 4094)
    call(sock, "utf8-write-suffix", "─")
PY
for _ in $(seq 1 100); do
    utf8_partial_response=$(api controller-read stdout --offset 14 --max 4096)
    if [ "$(json_field "$utf8_partial_response" result.next_offset)" = "4108" ]; then
        break
    fi
    sleep 0.05
done
if [ "$(json_field "$utf8_partial_response" result.length)" != "4094" ] ||
    [ "$(json_field "$utf8_partial_response" result.next_offset)" != "4108" ]; then
    echo "controller.read returned partial UTF-8 chunk: $utf8_partial_response" >&2
    exit 1
fi
utf8_full_response=$(api controller-read stdout --offset 4108 --max 3)
if [ "$(json_field "$utf8_full_response" result.data)" != "─" ] ||
    [ "$(json_field "$utf8_full_response" result.next_offset)" != "4111" ]; then
    echo "controller.read failed complete UTF-8 chunk: $utf8_full_response" >&2
    exit 1
fi

attach_response=$(api controller-attach local:test:process --channels 2)
if [ "$(json_field "$attach_response" success)" != "True" ]; then
    echo "controller.attach failed: $attach_response" >&2
    exit 1
fi
if [ "$(json_field "$attach_response" result.accepted_channels)" != "2" ]; then
    echo "controller.attach returned unexpected channels: $attach_response" >&2
    exit 1
fi
grep -q 'type=client_attached stream=stdout$' "$state_dir/events.log"

observer_stdin_response=$(api --allow-error controller-attach local:test:process --channels 1)
if [ "$(json_field "$observer_stdin_response" success)" != "False" ]; then
    echo "observer stdin attach unexpectedly succeeded: $observer_stdin_response" >&2
    exit 1
fi

write_response=$(api controller-write hello)
if [ "$(json_field "$write_response" success)" != "True" ]; then
    echo "controller.write failed: $write_response" >&2
    exit 1
fi

for _ in $(seq 1 100); do
    read_response=$(api controller-read stdout --offset 4111 --max 5)
    if [ "$(json_field "$read_response" result.data)" = "hello" ]; then
        break
    fi
    sleep 0.05
done

if [ "$(json_field "$read_response" result.data)" != "hello" ]; then
    echo "unexpected write/read response: $read_response" >&2
    exit 1
fi

detach_response=$(api controller-detach)
if [ "$(json_field "$detach_response" success)" != "True" ]; then
    echo "controller.detach failed: $detach_response" >&2
    exit 1
fi

line_status=$(python3 "$CUBICLE_CONTROL_CLIENT" "$socket_path" status)
case "$line_status" in
    ok\ state=running\ pid=*\ pgid=*\ stdout_offset=*\ stderr_offset=*) ;;
    *)
        echo "line protocol regressed: $line_status" >&2
        exit 1
        ;;
esac

python3 "$CUBICLE_CONTROL_CLIENT" "$socket_path" terminate >/dev/null
set +e
wait "$controller_pid"
status=$?
controller_pid=
set -e

if [ "$status" -ne 143 ]; then
    echo "expected terminated controller status 143, got $status" >&2
    exit 1
fi
