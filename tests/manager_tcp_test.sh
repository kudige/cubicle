set -eu

tmpdir=$(mktemp -d)
manager_pid=

cleanup() {
    if [ -n "${manager_pid:-}" ]; then
        python3 "$CUBICLE_API_CLIENT" "$endpoint" shutdown \
            >/dev/null 2>&1 || true
        wait "$manager_pid" 2>/dev/null || true
    fi
    rm -rf "$tmpdir"
}

trap cleanup EXIT

state_dir="$tmpdir/manager"
port=$(python3 - <<'PY'
import socket

with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    sock.bind(("127.0.0.1", 0))
    print(sock.getsockname()[1])
PY
)
endpoint="tcp://127.0.0.1:$port"

if "$CUBICLE_MANAGER" --state-dir "$tmpdir/rejected" daemon \
    --listen "$endpoint" --event-interval-ms 50 \
    >"$tmpdir/rejected.out" 2>"$tmpdir/rejected.err"; then
    echo "manager accepted insecure TCP without --allow-insecure" >&2
    exit 1
fi
grep -q -- '--allow-insecure' "$tmpdir/rejected.err"

"$CUBICLE_MANAGER" --state-dir "$state_dir" daemon \
    --listen "$endpoint" --allow-insecure --event-interval-ms 50 &
manager_pid=$!

for _ in $(seq 1 100); do
    if python3 - "$port" <<'PY'
import socket
import sys

with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    sock.settimeout(0.05)
    sock.connect(("127.0.0.1", int(sys.argv[1])))
PY
    then
        break
    fi
    sleep 0.05
done

# Endpoint test for unauthenticated TCP manager.ping
ping_response=$(python3 "$CUBICLE_API_CLIENT" "$endpoint" ping)
printf "%s" "$ping_response" | grep -q '"success": true'
printf "%s" "$ping_response" | grep -q '"protocol_major": 0'

# Endpoint test for unauthenticated TCP manager.status
status_response=$(python3 "$CUBICLE_API_CLIENT" "$endpoint" status)
printf "%s" "$status_response" | grep -q '"success": true'
printf "%s" "$status_response" | grep -q '"workspace_count": 0'

# CLI transport coverage for unauthenticated TCP. Protected methods should
# connect successfully and then fail authorization.
set +e
"$CUBE" --manager-socket "$endpoint" workspace "TCP Workspace" \
    >"$tmpdir/tcp-workspace.out" 2>"$tmpdir/tcp-workspace.err"
status=$?
set -e
if [ "$status" -ne 2 ]; then
    echo "unauthenticated TCP workspace list should fail authorization, got $status" >&2
    exit 1
fi
grep -q 'workspace creation requires local owner access' "$tmpdir/tcp-workspace.err"

# Endpoint test for unauthenticated TCP manager.shutdown
shutdown_response=$(python3 "$CUBICLE_API_CLIENT" "$endpoint" shutdown)
printf "%s" "$shutdown_response" | grep -q '"success": true'

wait "$manager_pid"
manager_pid=
