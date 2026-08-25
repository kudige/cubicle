set -eu

tmpdir=$(mktemp -d)
manager_pid=
local_manager_pid=
endpoint=
local_endpoint=

cleanup() {
    if [ -n "${local_manager_pid:-}" ] && [ -n "${local_endpoint:-}" ]; then
        XDG_CONFIG_HOME="$tmpdir/config" \
            XDG_STATE_HOME="$tmpdir/client-state" \
            XDG_RUNTIME_DIR="$tmpdir/runtime" \
            CUBICLE_MANAGER_SOCKET="$local_endpoint" \
            "$CUBE" shutdown >/dev/null 2>&1 || true
        wait "$local_manager_pid" 2>/dev/null || true
    fi
    if [ -n "${manager_pid:-}" ]; then
        if [ -n "${endpoint:-}" ]; then
            XDG_CONFIG_HOME="$tmpdir/config" \
                XDG_STATE_HOME="$tmpdir/client-state" \
                XDG_RUNTIME_DIR="$tmpdir/runtime" \
                CUBICLE_MANAGER_SOCKET="$endpoint" \
                "$CUBE" shutdown >/dev/null 2>&1 || true
        fi
        wait "$manager_pid" 2>/dev/null || true
    fi
    rm -rf "$tmpdir"
}

trap cleanup EXIT

mkdir -p "$tmpdir/runtime"
state_dir="$tmpdir/manager"
unix_socket="$tmpdir/manager.sock"

cube() {
    XDG_CONFIG_HOME="$tmpdir/config" \
        XDG_STATE_HOME="$tmpdir/client-state" \
        XDG_RUNTIME_DIR="$tmpdir/runtime" \
        "$CUBE" "$@"
}

client_key=$(cube identity pub)

"$CUBICLE_MANAGER" --state-dir "$state_dir" daemon \
    --foreground --control-socket "$unix_socket" --event-interval-ms 50 &
manager_pid=$!

for _ in $(seq 1 100); do
    [ -S "$unix_socket" ] && break
    sleep 0.05
done

CUBICLE_MANAGER_SOCKET="$unix_socket" cube workspace create Remote \
    >"$tmpdir/workspace-create.out"
CUBICLE_MANAGER_SOCKET="$unix_socket" cube --workspace Remote access add \
    "$client_key" --role owner --label tls-client >"$tmpdir/access-add.out"
CUBICLE_MANAGER_SOCKET="$unix_socket" cube shutdown >/dev/null
wait "$manager_pid"
manager_pid=

port=$(python3 - <<'PY'
import socket

with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    sock.bind(("127.0.0.1", 0))
    print(sock.getsockname()[1])
PY
)
endpoint="tls://127.0.0.1:$port"
tls_local_socket="$state_dir/manager.sock"

"$CUBICLE_MANAGER" --state-dir "$state_dir" daemon \
    --foreground --listen "$endpoint" --event-interval-ms 50 &
manager_pid=$!

for _ in $(seq 1 100); do
    [ -S "$tls_local_socket" ] || {
        sleep 0.05
        continue
    }
    if CUBICLE_MANAGER_SOCKET="$endpoint" cube workspace list \
        >"$tmpdir/tls-list.out" 2>"$tmpdir/tls-list.err"; then
        break
    fi
    sleep 0.05
done

grep -q 'Remote' "$tmpdir/tls-list.out"
CUBICLE_MANAGER_SOCKET="$tls_local_socket" cube workspace list \
    >"$tmpdir/tls-local-list.out"
grep -q 'Remote' "$tmpdir/tls-local-list.out"
test -f "$state_dir/tls/server.crt"
test -f "$state_dir/tls/server.key"

cube remote add lab "$endpoint" --yes >"$tmpdir/remote-add.out"
grep -q 'Manager public key:' "$tmpdir/remote-add.out"
cube remote list >"$tmpdir/remote-list.out"
grep -q "lab	$endpoint" "$tmpdir/remote-list.out"
cube remote inspect lab >"$tmpdir/remote-inspect.out"
grep -q "manager=$endpoint" "$tmpdir/remote-inspect.out"
cube remote remove lab
cube remote list >"$tmpdir/remote-list-empty.out"
grep -q 'No remotes configured' "$tmpdir/remote-list-empty.out"
cube remote add lab "$endpoint" --yes >"$tmpdir/remote-add-again.out"

local_state_dir="$tmpdir/local-manager"
local_socket="$tmpdir/local-manager.sock"
local_endpoint="$local_socket"
"$CUBICLE_MANAGER" --state-dir "$local_state_dir" daemon \
    --foreground --control-socket "$local_socket" --event-interval-ms 50 &
local_manager_pid=$!

for _ in $(seq 1 100); do
    [ -S "$local_socket" ] && break
    sleep 0.05
done

CUBICLE_MANAGER_SOCKET="$local_socket" cube workspace create Local \
    >"$tmpdir/local-workspace-create.out"
CUBICLE_MANAGER_SOCKET="$local_socket" cube ps -a >"$tmpdir/ps-all.out"
grep -q 'Workspace Local' "$tmpdir/ps-all.out"
grep -q 'Workspace Remote@lab' "$tmpdir/ps-all.out"

CUBICLE_MANAGER_SOCKET="$local_socket" cube shutdown >/dev/null
wait "$local_manager_pid"
local_manager_pid=
local_endpoint=

CUBICLE_MANAGER_SOCKET="$endpoint" cube shutdown >/dev/null
wait "$manager_pid"
manager_pid=
