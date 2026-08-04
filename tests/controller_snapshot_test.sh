set -eu

tmpdir=$(mktemp -d)
trap 'if [ -n "${stream_pid:-}" ]; then kill "$stream_pid" 2>/dev/null || true; wait "$stream_pid" 2>/dev/null || true; fi; if [ -n "${tty_pid:-}" ]; then kill "$tty_pid" 2>/dev/null || true; wait "$tty_pid" 2>/dev/null || true; fi; if [ -n "${large_pid:-}" ]; then kill "$large_pid" 2>/dev/null || true; wait "$large_pid" 2>/dev/null || true; fi; rm -rf "$tmpdir"' EXIT

json_field() {
    printf '%s' "$1" | python3 -c 'import json,sys
document = json.load(sys.stdin)
value = document
for part in sys.argv[1].split("."):
    value = value[part]
print(value)' "$2"
}

json_cell_text() {
    printf '%s' "$1" | python3 -c 'import json,sys
document = json.load(sys.stdin)
row = int(sys.argv[1])
col = int(sys.argv[2])
result = document["result"]
index = row * result["columns"] + col
print(result["cells"][index]["t"])' "$2" "$3"
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

large_state="$tmpdir/large-state"
large_socket="$tmpdir/large.sock"
"$CUBICLE_CONTROLLER" --state-dir "$large_state" --control-socket "$large_socket" --mode tty -- python3 -c 'import sys,time; time.sleep(0.5); sys.stdout.write("\033[2J"); [sys.stdout.write("\033[%d;%dH\033[38;5;%dmX" % (r, c, (r * c) % 256)) for r in range(1, 41) for c in range(1, 121)]; sys.stdout.flush(); time.sleep(30)' >/dev/null 2>/dev/null &
large_pid=$!
wait_for_socket "$large_socket"

large_resize=$(api "$large_socket" controller-resize 40 120)
if [ "$(json_field "$large_resize" success)" != "True" ]; then
    echo "large controller resize failed: $large_resize" >&2
    exit 1
fi

large_snapshot=""
for _ in $(seq 1 100); do
    large_snapshot=$(api "$large_socket" call controller.snapshot)
    if [ "$(json_cell_text "$large_snapshot" 39 119)" = "X" ]; then
        break
    fi
    sleep 0.05
done

if [ "$(json_field "$large_snapshot" success)" != "True" ]; then
    echo "large controller.snapshot failed: $large_snapshot" >&2
    exit 1
fi
if [ "$(json_cell_text "$large_snapshot" 39 119)" != "X" ]; then
    echo "large controller.snapshot returned unexpected cells" >&2
    exit 1
fi

python3 "$CUBICLE_CONTROL_CLIENT" "$large_socket" terminate >/dev/null
set +e
wait "$large_pid"
large_status=$?
large_pid=
set -e

if [ "$large_status" -ne 143 ]; then
    echo "expected terminated large TTY controller status 143, got $large_status" >&2
    exit 1
fi
