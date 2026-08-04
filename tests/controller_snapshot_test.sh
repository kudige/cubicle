set -eu

tmpdir=$(mktemp -d)
trap 'if [ -n "${stream_pid:-}" ]; then kill "$stream_pid" 2>/dev/null || true; wait "$stream_pid" 2>/dev/null || true; fi; if [ -n "${tty_pid:-}" ]; then kill "$tty_pid" 2>/dev/null || true; wait "$tty_pid" 2>/dev/null || true; fi; rm -rf "$tmpdir"' EXIT

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

json_cell_text() {
    python3 - "$1" "$2" "$3" <<'PY'
import json
import sys

document = json.loads(sys.argv[1])
row = int(sys.argv[2])
col = int(sys.argv[3])
result = document["result"]
index = row * result["columns"] + col
print(result["cells"][index]["t"])
PY
}

api() {
    endpoint=$1
    shift
    python3 "$CUBICLE_API_CLIENT" --raw "$endpoint" "$@"
}

wait_for_socket() {
    socket_path=$1
    for _ in $(seq 1 100); do
        if [ -S "$socket_path" ]; then
            return 0
        fi
        sleep 0.05
    done
    echo "control socket was not created: $socket_path" >&2
    exit 1
}

stream_state="$tmpdir/stream-state"
stream_socket="$tmpdir/stream.sock"
"$CUBICLE_CONTROLLER" --state-dir "$stream_state" --control-socket "$stream_socket" --mode stream -- sh -c 'printf stream-ready; sleep 30' >/dev/null 2>/dev/null &
stream_pid=$!
wait_for_socket "$stream_socket"

stream_snapshot=$(api "$stream_socket" --allow-error call controller.snapshot)
if [ "$(json_field "$stream_snapshot" success)" != "False" ]; then
    echo "stream controller.snapshot unexpectedly succeeded: $stream_snapshot" >&2
    exit 1
fi

python3 "$CUBICLE_CONTROL_CLIENT" "$stream_socket" terminate >/dev/null
set +e
wait "$stream_pid"
stream_pid=
set -e

tty_state="$tmpdir/tty-state"
tty_socket="$tmpdir/tty.sock"
"$CUBICLE_CONTROLLER" --state-dir "$tty_state" --control-socket "$tty_socket" --mode tty -- sh -c 'printf "\033[2J\033[2;3Hhello"; sleep 30' >/dev/null 2>/dev/null &
tty_pid=$!
wait_for_socket "$tty_socket"

snapshot=""
for _ in $(seq 1 100); do
    snapshot=$(api "$tty_socket" call controller.snapshot)
    if [ "$(json_cell_text "$snapshot" 1 2)" = "h" ]; then
        break
    fi
    sleep 0.05
done

if [ "$(json_field "$snapshot" success)" != "True" ]; then
    echo "controller.snapshot failed: $snapshot" >&2
    exit 1
fi
if [ "$(json_cell_text "$snapshot" 1 2)" != "h" ] ||
   [ "$(json_cell_text "$snapshot" 1 3)" != "e" ] ||
   [ "$(json_cell_text "$snapshot" 1 4)" != "l" ]; then
    echo "controller.snapshot returned unexpected cells: $snapshot" >&2
    exit 1
fi
if [ "$(json_field "$snapshot" result.offset)" -le 0 ]; then
    echo "controller.snapshot returned invalid offset: $snapshot" >&2
    exit 1
fi

"$CUBICLE_ATTACHMENT_SNAPSHOT_CLIENT" "$tty_socket"

python3 "$CUBICLE_CONTROL_CLIENT" "$tty_socket" terminate >/dev/null
set +e
wait "$tty_pid"
tty_status=$?
tty_pid=
set -e

if [ "$tty_status" -ne 143 ]; then
    echo "expected terminated TTY controller status 143, got $tty_status" >&2
    exit 1
fi
