set -eu

tmpdir=$(mktemp -d)
manager_started=0

cleanup() {
    if [ "$manager_started" -eq 1 ] && [ -S "$socket_path" ]; then
        CUBICLE_CONFIG="$config_file" python3 "$CUBICLE_API_CLIENT" "$socket_path" shutdown \
            >/dev/null 2>&1 || true
    fi
    rm -rf "$tmpdir"
}

trap cleanup EXIT

manager_bindir=$(cd "$(dirname "$CUBICLE_MANAGER")" && pwd)
state_dir="$tmpdir/state"
runtime_dir="$tmpdir/run"
log_dir="$tmpdir/log"
socket_path="$runtime_dir/manager.sock"
config_file="$tmpdir/config.cfg"
disabled_config="$tmpdir/disabled.cfg"
desk_config="$tmpdir/desk.cfg"
desk_disabled_config="$tmpdir/desk-disabled.cfg"
xdg_state="$tmpdir/xdg-state"

cat >"$config_file" <<EOF
[installation]
bindir=$manager_bindir

[manager]
state_dir=$state_dir
runtime_dir=$runtime_dir
log_dir=$log_dir
listen=unix://$socket_path

[client]
manager=unix://$socket_path

[cube]
automanager=true
EOF

XDG_STATE_HOME="$xdg_state" "$CUBE" --config "$config_file" workspace Auto \
    >"$tmpdir/workspace.out"
manager_started=1
grep -q '^Workspace Auto created and selected$' "$tmpdir/workspace.out"
if [ ! -S "$socket_path" ]; then
    echo "cube automanager did not start manager socket" >&2
    exit 1
fi
python3 "$CUBICLE_API_CLIENT" "$socket_path" ping >/dev/null

CUBICLE_CONFIG="$config_file" python3 "$CUBICLE_API_CLIENT" "$socket_path" shutdown \
    >/dev/null
for _ in $(seq 1 100); do
    if [ ! -S "$socket_path" ]; then
        break
    fi
    sleep 0.05
done
manager_started=0

cat >"$disabled_config" <<EOF
[installation]
bindir=$manager_bindir

[manager]
state_dir=$tmpdir/disabled-state
runtime_dir=$tmpdir/disabled-run
log_dir=$tmpdir/disabled-log
listen=unix://$tmpdir/disabled-run/manager.sock

[client]
manager=unix://$tmpdir/disabled-run/manager.sock

[cube]
automanager=false
EOF

set +e
XDG_STATE_HOME="$tmpdir/disabled-xdg-state" "$CUBE" --config "$disabled_config" workspace list \
    >"$tmpdir/disabled.out" 2>"$tmpdir/disabled.err"
status=$?
set -e
if [ "$status" -ne 2 ]; then
    echo "cube with disabled automanager should fail with status 2, got $status" >&2
    exit 1
fi
grep -q 'failed to connect to manager' "$tmpdir/disabled.err"
if [ -S "$tmpdir/disabled-run/manager.sock" ]; then
    echo "disabled automanager unexpectedly created a manager socket" >&2
    exit 1
fi

desk_socket="$tmpdir/desk-run/manager.sock"
cat >"$desk_config" <<EOF
[installation]
bindir=$manager_bindir

[manager]
state_dir=$tmpdir/desk-state
runtime_dir=$tmpdir/desk-run
log_dir=$tmpdir/desk-log
listen=unix://$desk_socket

[client]
manager=unix://$desk_socket

[desk]
automanager=true
EOF

set +e
"$DESK" --config "$desk_config" --workspace Auto \
    >"$tmpdir/desk.out" 2>"$tmpdir/desk.err"
desk_status=$?
set -e
if [ "$desk_status" -eq 0 ]; then
    echo "desk without a tty should not exit successfully" >&2
    exit 1
fi
if [ ! -S "$desk_socket" ]; then
    echo "desk automanager did not start manager socket" >&2
    cat "$tmpdir/desk.err" >&2 || true
    exit 1
fi
CUBICLE_CONFIG="$desk_config" python3 "$CUBICLE_API_CLIENT" "$desk_socket" shutdown \
    >/dev/null
for _ in $(seq 1 100); do
    if [ ! -S "$desk_socket" ]; then
        break
    fi
    sleep 0.05
done

cat >"$desk_disabled_config" <<EOF
[installation]
bindir=$manager_bindir

[manager]
state_dir=$tmpdir/desk-disabled-state
runtime_dir=$tmpdir/desk-disabled-run
log_dir=$tmpdir/desk-disabled-log
listen=unix://$tmpdir/desk-disabled-run/manager.sock

[client]
manager=unix://$tmpdir/desk-disabled-run/manager.sock

[desk]
automanager=false
EOF

set +e
"$DESK" --config "$desk_disabled_config" --workspace Auto \
    >"$tmpdir/desk-disabled.out" 2>"$tmpdir/desk-disabled.err"
desk_disabled_status=$?
set -e
if [ "$desk_disabled_status" -ne 2 ]; then
    echo "desk with disabled automanager should fail with status 2, got $desk_disabled_status" >&2
    exit 1
fi
grep -q 'failed to connect to manager' "$tmpdir/desk-disabled.err"
if [ -S "$tmpdir/desk-disabled-run/manager.sock" ]; then
    echo "disabled desk automanager unexpectedly created a manager socket" >&2
    exit 1
fi
