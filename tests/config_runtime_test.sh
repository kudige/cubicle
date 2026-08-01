set -eu

tmpdir=$(mktemp -d)
manager_pid=

cleanup() {
    if [ -n "${manager_pid:-}" ]; then
        CUBICLE_CONFIG="$config_file" python3 "$CUBICLE_API_CLIENT" "$socket_path" shutdown \
            >/dev/null 2>&1 || true
        wait "$manager_pid" 2>/dev/null || true
    fi
    rm -rf "$tmpdir"
}

trap cleanup EXIT

state_dir="$tmpdir/state"
runtime_dir="$tmpdir/run"
log_dir="$tmpdir/log"
socket_path="$runtime_dir/manager.sock"
config_file="$tmpdir/config.cfg"
xdg_state="$tmpdir/xdg-state"
controller_binary=$(cd "$(dirname "$CUBICLE_CONTROLLER")" && pwd)/$(basename "$CUBICLE_CONTROLLER")

cat >"$config_file" <<EOF
[manager]
state_dir=$state_dir
runtime_dir=$runtime_dir
log_dir=$log_dir
listen=unix://$socket_path
controller_binary=$controller_binary

[client]
manager=unix://$socket_path

[defaults]
launch=background
mode=term
EOF

CUBICLE_CONFIG="$config_file" "$CUBICLE_MANAGER" daemon --event-interval-ms 50 &
manager_pid=$!

for _ in $(seq 1 100); do
    if [ -S "$socket_path" ]; then
        break
    fi
    sleep 0.05
done

if [ ! -S "$socket_path" ]; then
    echo "manager did not create configured runtime socket" >&2
    exit 1
fi

[ -d "$state_dir" ]
[ -d "$runtime_dir" ]
[ -d "$log_dir" ]

CUBICLE_CONFIG="$config_file" XDG_STATE_HOME="$xdg_state" "$CUBE" config validate \
    >"$tmpdir/config-validate.out"
grep -q '^configuration valid$' "$tmpdir/config-validate.out"

CUBICLE_CONFIG="$config_file" XDG_STATE_HOME="$xdg_state" "$CUBE" config paths \
    >"$tmpdir/config-paths.out"
grep -q "^manager.state_dir=$state_dir$" "$tmpdir/config-paths.out"
grep -q "^manager.runtime_dir=$runtime_dir$" "$tmpdir/config-paths.out"
grep -q "^manager.log_dir=$log_dir$" "$tmpdir/config-paths.out"

CUBICLE_CONFIG="$config_file" XDG_STATE_HOME="$xdg_state" "$CUBE" workspace "Project A" \
    >"$tmpdir/workspace.out"
workspace_json=$(python3 "$CUBICLE_API_CLIENT" "$socket_path" call workspace.get '{"workspace":"Project A"}')
workspace_id=$(python3 - "$workspace_json" <<'PY'
import json
import sys
print(json.loads(sys.argv[1])["result"]["id"])
PY
)

CUBICLE_CONFIG="$config_file" XDG_STATE_HOME="$xdg_state" "$CUBE" run --name config-run \
    sh -c 'test -t 0 && test -t 1 && ! test -t 2; printf "config-out\n"; printf "config-err\n" >&2' \
    >"$tmpdir/run.out" 2>"$tmpdir/run.err"
grep -q '^\[config-run\] started in term mode$' "$tmpdir/run.out"

for _ in $(seq 1 100); do
    process_json=$(python3 "$CUBICLE_API_CLIENT" "$socket_path" call process.get "{\"process\":\"config-run\",\"workspace_id\":\"$workspace_id\"}")
    process_id=$(python3 - "$process_json" <<'PY'
import json
import sys
print(json.loads(sys.argv[1])["result"]["id"])
PY
)
    if [ -n "$process_id" ] &&
        [ -f "$log_dir/controllers/$process_id/stdout.log" ] &&
        grep -q 'config-out' "$log_dir/controllers/$process_id/stdout.log" &&
        grep -q 'config-err' "$log_dir/controllers/$process_id/stderr.log"; then
        break
    fi
    sleep 0.05
done

[ -f "$state_dir/controllers/$process_id/metadata" ]
[ -d "$runtime_dir/controllers/$process_id" ]
[ -f "$log_dir/controllers/$process_id/events.log" ]
[ -f "$log_dir/controllers/$process_id/stdout.log" ]
[ -f "$log_dir/controllers/$process_id/stderr.log" ]

if [ -f "$state_dir/controllers/$process_id/stdout.log" ] ||
    [ -f "$state_dir/controllers/$process_id/stderr.log" ] ||
    [ -f "$state_dir/controllers/$process_id/events.log" ]; then
    echo "controller logs should be stored under configured log_dir" >&2
    exit 1
fi

CUBICLE_CONFIG="$config_file" XDG_STATE_HOME="$xdg_state" "$CUBE" logs config-run \
    >"$tmpdir/logs.out" 2>"$tmpdir/logs.err"
grep -q 'config-out' "$tmpdir/logs.out"
grep -q 'config-err' "$tmpdir/logs.err"

python3 "$CUBICLE_API_CLIENT" "$socket_path" shutdown >/dev/null 2>&1 || true
wait "$manager_pid" 2>/dev/null || true
manager_pid=

user_state_home="$tmpdir/user-state"
user_runtime_dir="$tmpdir/user-run"
user_socket="$user_runtime_dir/cubicle/manager.sock"
XDG_STATE_HOME="$user_state_home" XDG_RUNTIME_DIR="$user_runtime_dir" \
    "$CUBICLE_MANAGER" --controller-bin "$controller_binary" daemon \
    --event-interval-ms 50 &
manager_pid=$!

for _ in $(seq 1 100); do
    if [ -S "$user_socket" ]; then
        break
    fi
    sleep 0.05
done

if [ ! -S "$user_socket" ]; then
    echo "manager did not create default per-user socket" >&2
    exit 1
fi

XDG_STATE_HOME="$user_state_home" XDG_RUNTIME_DIR="$user_runtime_dir" \
    "$CUBE" config paths >"$tmpdir/user-config-paths.out"
grep -q "^manager.state_dir=$user_state_home/cubicle$" "$tmpdir/user-config-paths.out"
grep -q "^manager.runtime_dir=$user_runtime_dir/cubicle$" "$tmpdir/user-config-paths.out"
grep -q "^manager.log_dir=$user_state_home/cubicle/log$" "$tmpdir/user-config-paths.out"

kill "$manager_pid" 2>/dev/null || true
wait "$manager_pid" 2>/dev/null || true
manager_pid=
