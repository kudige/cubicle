set -eu

tmpdir=$(mktemp -d)
trap 'if [ -n "${controller_pid:-}" ]; then kill "$controller_pid" 2>/dev/null || true; wait "$controller_pid" 2>/dev/null || true; fi; rm -rf "$tmpdir"' EXIT

state_dir="$tmpdir/state"
socket_path="$tmpdir/control.sock"

"$CUBICLE_CONTROLLER" --debug input,library --state-dir "$state_dir" \
    --control-socket "$socket_path" --mode stream -- \
    sh -c 'cat >/dev/null; sleep 30' >/dev/null 2>/dev/null &
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

response=$(api controller-write "$(printf '\033[10;1R\033]10;rgb:e3e3/e3e3/eaea\033\\\\\033[?1;2;4c')")
python3 - "$response" <<'PY'
import json
import sys

document = json.loads(sys.argv[1])
if not document.get("success"):
    raise SystemExit(document)
PY

if ! grep -q 'type=input length=.* source=api .*data_hex=' "$state_dir/events.log"; then
    echo "debug input event did not include source/data_hex" >&2
    cat "$state_dir/events.log" >&2
    exit 1
fi

if ! grep -Fq 'data_escaped=\e[10;1R' "$state_dir/events.log" ||
   ! grep -Fq 'rgb:e3e3/e3e3/eaea' "$state_dir/events.log" ||
   ! grep -Fq '\e[?1;2;4c' "$state_dir/events.log"; then
    echo "debug input event did not include escaped terminal bytes" >&2
    cat "$state_dir/events.log" >&2
    exit 1
fi

if ! grep -q 'type=debug category=library event=api_response method=controller.write ' "$state_dir/events.log"; then
    echo "library debug event did not include controller.write" >&2
    cat "$state_dir/events.log" >&2
    exit 1
fi
