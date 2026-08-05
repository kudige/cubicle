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
socket_mode=0664
controller_binary=$controller_binary

[client]
manager=unix://$socket_path

[defaults]
launch=background
mode=term
EOF

mkdir -p "$config_file.d"
cat >"$config_file.d/90-cube-debug.cfg" <<EOF
[cube]
debug=library
EOF

"$CUBICLE_MANAGER" --config "$config_file" daemon --foreground --event-interval-ms 50 &
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
test "$(stat -c '%a' "$state_dir")" = "700"
test "$(stat -c '%a' "$runtime_dir")" = "700"
test "$(stat -c '%a' "$log_dir")" = "700"
socket_mode=$(stat -c '%a' "$socket_path")
if [ "$socket_mode" != "664" ]; then
    echo "configured socket mode was not applied: $socket_mode" >&2
    exit 1
fi

XDG_STATE_HOME="$xdg_state" "$CUBE" --config "$config_file" config validate \
    >"$tmpdir/config-validate.out"
grep -q '^configuration valid$' "$tmpdir/config-validate.out"

XDG_STATE_HOME="$xdg_state" "$CUBE" --config "$config_file" config paths \
    >"$tmpdir/config-paths.out"
grep -q "^manager.state_dir=$state_dir$" "$tmpdir/config-paths.out"
grep -q "^manager.runtime_dir=$runtime_dir$" "$tmpdir/config-paths.out"
grep -q "^manager.log_dir=$log_dir$" "$tmpdir/config-paths.out"
grep -q '^manager.socket_mode=0664$' "$tmpdir/config-paths.out"

XDG_STATE_HOME="$xdg_state" "$CUBE" --config "$config_file" config effective \
    >"$tmpdir/config-effective.out"
grep -q '^Configuration sources:$' "$tmpdir/config-effective.out"
grep -q "^  $config_file$" "$tmpdir/config-effective.out"
grep -q "^  manager.state_dir .* $state_dir$" "$tmpdir/config-effective.out"
grep -q "^      source: $config_file (override)$" "$tmpdir/config-effective.out"
grep -q "^  $config_file.d/90-cube-debug.cfg$" "$tmpdir/config-effective.out"
grep -q '^  cube.debug .* library$' "$tmpdir/config-effective.out"
grep -q "^      source: $config_file.d/90-cube-debug.cfg (override)$" "$tmpdir/config-effective.out"
grep -q '^  desk.debug .* none$' "$tmpdir/config-effective.out"
grep -q '^      source: built-in defaults (built-in)$' "$tmpdir/config-effective.out"

XDG_STATE_HOME="$xdg_state" "$CUBE" --config "$config_file" workspace "Project A" \
    >"$tmpdir/workspace.out"
workspace_json=$(python3 "$CUBICLE_API_CLIENT" "$socket_path" call workspace.get '{"workspace":"Project A"}')
workspace_id=$(python3 - "$workspace_json" <<'PY'
import json
import sys
print(json.loads(sys.argv[1])["result"]["id"])
PY
)

XDG_STATE_HOME="$xdg_state" "$CUBE" --config "$config_file" run --name config-run \
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

XDG_STATE_HOME="$xdg_state" "$CUBE" --config "$config_file" logs config-run \
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
    "$CUBICLE_MANAGER" --controller-bin "$controller_binary" daemon --foreground \
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

defaults_config_home="$tmpdir/defaults-config"
defaults_config="$defaults_config_home/cubicle/config.cfg"
mkdir -p "$defaults_config_home/cubicle"
cat >"$defaults_config" <<EOF
[client]
manager=unix:///tmp/preserved-manager.sock
EOF

XDG_CONFIG_HOME="$defaults_config_home" XDG_STATE_HOME="$xdg_state" \
    "$CUBE" defaults set launch background >"$tmpdir/defaults-set-launch.out"
grep -q '^defaults.launch=background$' "$tmpdir/defaults-set-launch.out"
grep -q '^manager=unix:///tmp/preserved-manager.sock$' "$defaults_config"
grep -q '^\[defaults\]$' "$defaults_config"
grep -q '^launch=background$' "$defaults_config"

XDG_CONFIG_HOME="$defaults_config_home" XDG_STATE_HOME="$xdg_state" \
    "$CUBE" defaults set mode stream >"$tmpdir/defaults-set-mode.out"
XDG_CONFIG_HOME="$defaults_config_home" XDG_STATE_HOME="$xdg_state" \
    "$CUBE" defaults set kill-cleanup true >"$tmpdir/defaults-set-cleanup.out"

XDG_CONFIG_HOME="$defaults_config_home" XDG_STATE_HOME="$xdg_state" \
    "$CUBE" defaults show >"$tmpdir/defaults-show.out"
grep -q '^launch=background$' "$tmpdir/defaults-show.out"
grep -q '^mode=stream$' "$tmpdir/defaults-show.out"
grep -q '^kill_cleanup=true$' "$tmpdir/defaults-show.out"

XDG_CONFIG_HOME="$defaults_config_home" XDG_STATE_HOME="$xdg_state" \
    "$CUBE" defaults reset mode >"$tmpdir/defaults-reset-mode.out"
grep -q '^defaults.mode reset$' "$tmpdir/defaults-reset-mode.out"
if grep -q '^mode=' "$defaults_config"; then
    echo "defaults reset mode should remove the user mode override" >&2
    exit 1
fi
grep -q '^launch=background$' "$defaults_config"
grep -q '^kill_cleanup=true$' "$defaults_config"

XDG_CONFIG_HOME="$defaults_config_home" XDG_STATE_HOME="$xdg_state" \
    "$CUBE" defaults reset >"$tmpdir/defaults-reset-all.out"
grep -q '^defaults.all reset$' "$tmpdir/defaults-reset-all.out"
if grep -q '^launch=' "$defaults_config" ||
    grep -q '^mode=' "$defaults_config" ||
    grep -q '^kill_cleanup=' "$defaults_config"; then
    echo "defaults reset should remove all user default overrides" >&2
    exit 1
fi
grep -q '^manager=unix:///tmp/preserved-manager.sock$' "$defaults_config"

unsafe_config="$tmpdir/unsafe.cfg"
unsafe_state_dir="$tmpdir/unsafe-state"
mkdir -p "$unsafe_state_dir"
chmod 0777 "$unsafe_state_dir"
cat >"$unsafe_config" <<EOF
[manager]
state_dir=$unsafe_state_dir
runtime_dir=$tmpdir/unsafe-runtime
log_dir=$tmpdir/unsafe-log
listen=unix://$tmpdir/unsafe-runtime/manager.sock
controller_binary=$controller_binary
EOF

if "$CUBICLE_MANAGER" --config "$unsafe_config" workspace list \
    >"$tmpdir/unsafe.out" 2>"$tmpdir/unsafe.err"; then
    echo "manager accepted an unsafe configured state directory" >&2
    exit 1
fi
grep -q "manager.state_dir ($unsafe_state_dir): must not be writable by group or other" \
    "$tmpdir/unsafe.err"
