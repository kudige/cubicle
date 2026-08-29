set -eu

tmpdir=$(mktemp -d)
manager_pid=

cleanup() {
    if [ -n "${manager_pid:-}" ]; then
        python3 "$CUBICLE_API_CLIENT" "$socket_path" shutdown \
            >/dev/null 2>&1 || true
        wait "$manager_pid" 2>/dev/null || true
    fi
    rm -rf "$tmpdir"
}

trap cleanup EXIT

state_dir="$tmpdir/manager"
socket_path="$tmpdir/manager.sock"
xdg_state_home="$tmpdir/xdg-state"
xdg_runtime_dir="$tmpdir/xdg-runtime"
xdg_config_home="$tmpdir/xdg-config"
client_log_dir="$tmpdir/client-log"
mkdir -p "$xdg_runtime_dir" "$xdg_config_home/cubicle"
cat >"$xdg_config_home/cubicle/config.cfg" <<EOF
[manager]
log_dir=$client_log_dir

[cube]
debug=library
EOF
workspace_dir="$tmpdir/workspace-dir"
mkdir -p "$workspace_dir"
workspace_dir=$(cd "$workspace_dir" && pwd -P)

"$CUBICLE_MANAGER" --state-dir "$state_dir" daemon --foreground \
    --control-socket "$socket_path" --event-interval-ms 50 &
manager_pid=$!

for _ in $(seq 1 100); do
    if [ -S "$socket_path" ]; then
        break
    fi
    sleep 0.05
done

if [ ! -S "$socket_path" ]; then
    echo "manager daemon did not create control socket" >&2
    exit 1
fi

cube() {
    XDG_STATE_HOME="$xdg_state_home" \
        XDG_RUNTIME_DIR="$xdg_runtime_dir" \
        XDG_CONFIG_HOME="$xdg_config_home" \
        CUBICLE_MANAGER_SOCKET="$socket_path" \
        "$CUBE" "$@"
}

output=$(cube workspace create "Project A")
if [ "$output" != "Workspace Project A created and selected" ]; then
    echo "unexpected workspace create output: $output" >&2
    exit 1
fi
if [ ! -f "$client_log_dir/client-library.log" ]; then
    echo "cube.debug=library did not create client library log" >&2
    exit 1
fi
grep -q 'program=cube ' "$client_log_dir/client-library.log"
grep -q 'event=rpc.request method=workspace.create code=ok' "$client_log_dir/client-library.log"
grep -q 'event=rpc.response method=workspace.create code=ok' "$client_log_dir/client-library.log"

json_dir_create_output=$(cube --json workspace create --dir "$workspace_dir" "Project Dir")
printf "%s" "$json_dir_create_output" | grep -q '"name":"Project Dir"'
printf "%s" "$json_dir_create_output" | grep -q "\"directory\":\"$workspace_dir\""
cube workspace select "Project A" >/dev/null

owner_key=$(tr -d '\n' <"$xdg_config_home/cubicle/keys/client.pub")
manager_access_list=$(cube access list)
printf "%s\n" "$manager_access_list" | grep -q '^KEY ID	LEVEL	LABEL	CAPABILITIES	REVOKED$'
printf "%s\n" "$manager_access_list" | grep -q '	Project A	owner	.*	no$'
access_list=$(cube --workspace "Project A" access list)
printf "%s\n" "$access_list" | grep -q '^KEY ID	LEVEL	LABEL	CAPABILITIES	REVOKED$'
printf "%s\n" "$access_list" | grep -q '	Project A	owner	.*	no$'
all_access_list=$(cube access list)
printf "%s\n" "$all_access_list" | grep -q '^KEY ID	LEVEL	LABEL	CAPABILITIES	REVOKED$'
printf "%s\n" "$all_access_list" | grep -q '	Project A	owner	.*	no$'
access_json=$(cube --json --workspace "Project A" access list)
printf "%s" "$access_json" | grep -q '"keys"'
printf "%s" "$access_json" | grep -q '"label":"owner"'

second_key=000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f
add_json=$(cube --json --workspace "Project A" access add "$second_key" --role observer --label "Alice")
printf "%s" "$add_json" | grep -q '"label":"Alice"'
second_key_id=$(python3 - "$add_json" <<'PY'
import json
import sys
print(json.loads(sys.argv[1])["key_id"])
PY
)
update_output=$(cube --workspace "Project A" access set-role "$second_key_id" operator)
if [ "$update_output" != "Access updated" ]; then
    echo "unexpected access set-role output: $update_output" >&2
    exit 1
fi
remove_output=$(cube --workspace "Project A" access remove "$second_key_id")
if [ "$remove_output" != "Access removed" ]; then
    echo "unexpected access remove output: $remove_output" >&2
    exit 1
fi
access_after_remove=$(cube --json --workspace "Project A" access list)
printf "%s" "$access_after_remove" | grep -q "\"key_id\":\"$second_key_id\""
printf "%s" "$access_after_remove" | grep -q "\"revoked_at_ms\":[1-9]"
manager_add_json=$(cube --json access add "$second_key" --role owner --label "Global Alice")
printf "%s" "$manager_add_json" | grep -q '"label":"Global Alice"'
printf "%s" "$manager_add_json" | grep -q '"scope":"manager"'
manager_key_id=$(python3 - "$manager_add_json" <<'PY'
import json
import sys
print(json.loads(sys.argv[1])["key_id"])
PY
)
manager_access_json=$(cube --json access list)
printf "%s" "$manager_access_json" | grep -q "\"key_id\":\"$manager_key_id\""
printf "%s" "$manager_access_json" | grep -q '"scope":"manager"'
printf "%s" "$manager_access_json" | grep -q '"scope":"workspace"'
printf "%s" "$manager_access_json" | grep -q '"workspace_name":"Project A"'
manager_access_list=$(cube access list)
printf "%s\n" "$manager_access_list" | grep -q "	global	Global Alice	"
printf "%s\n" "$manager_access_list" | grep -q "	Project A	owner	"
manager_update_output=$(cube access set-role "$manager_key_id" operator)
if [ "$manager_update_output" != "Access updated" ]; then
    echo "unexpected manager access set-role output: $manager_update_output" >&2
    exit 1
fi
manager_remove_output=$(cube access remove "$manager_key_id")
if [ "$manager_remove_output" != "Access removed" ]; then
    echo "unexpected manager access remove output: $manager_remove_output" >&2
    exit 1
fi
printf "%s" "$owner_key" | grep -Eq '^[0-9a-f]{64}$'

json_create_output=$(cube --json workspace create "Project JSON")
printf "%s" "$json_create_output" | grep -q '"name":"Project JSON"'
printf "%s" "$json_create_output" | grep -q '"id"'

json_current_output=$(cube --json workspace)
printf "%s" "$json_current_output" | grep -q '"workspace":"Project JSON"'

json_select_output=$(cube --json workspace select "Project A")
printf "%s" "$json_select_output" | grep -q '"name":"Project A"'

output=$(cube workspace)
if [ "$output" != "Workspace Project A selected" ]; then
    echo "unexpected selected workspace output: $output" >&2
    exit 1
fi

mkdir -p "$xdg_state_home/cubicle"
printf "Missing Workspace\n" >"$xdg_state_home/cubicle/current-workspace"
set +e
cube ps >"$tmpdir/stale-selected.out" 2>"$tmpdir/stale-selected.err"
status=$?
set -e
if [ "$status" -ne 1 ]; then
    echo "stale selected workspace should exit 1, got $status" >&2
    exit 1
fi
grep -q "selected workspace 'Missing Workspace' was not found by the manager" "$tmpdir/stale-selected.err"
grep -q 'cube workspace list' "$tmpdir/stale-selected.err"
if [ -e "$xdg_state_home/cubicle/current-workspace" ]; then
    echo "stale selected workspace should be cleared" >&2
    exit 1
fi
cube workspace select "Project A" >/dev/null

session_status=$(python3 "$CUBICLE_API_CLIENT" "$socket_path" status)
printf "%s" "$session_status" | grep -q '"active_client_sessions": 1'

output=$(cube workspace "Project A")
if [ "$output" != "Workspace Project A selected" ]; then
    echo "unexpected workspace select output: $output" >&2
    exit 1
fi

output=$(cube --manager-socket "$socket_path" workspace "Project B")
if [ "$output" != "Workspace Project B created and selected" ]; then
    echo "unexpected explicit-socket workspace output: $output" >&2
    exit 1
fi

list_output=$(cube workspace list)
printf "%s\n" "$list_output" | grep -q '^WORKSPACE ID	NAME$'
printf "%s\n" "$list_output" | grep -q '	Project A$'
printf "%s\n" "$list_output" | grep -q '	Project B$'

json_list_output=$(cube --json workspace list)
printf "%s" "$json_list_output" | grep -q '"workspaces"'
printf "%s" "$json_list_output" | grep -q '"name":"Project A"'
printf "%s" "$json_list_output" | grep -q '"name":"Project B"'
printf "%s" "$json_list_output" | grep -q "\"directory\":\"$workspace_dir\""

json_stop_output=$(cube --json workspace stop "Project JSON")
if [ "$json_stop_output" != "{}" ]; then
    echo "unexpected workspace stop JSON output: $json_stop_output" >&2
    exit 1
fi

json_delete_output=$(cube --json workspace delete "Project JSON")
if [ "$json_delete_output" != "{}" ]; then
    echo "unexpected workspace delete JSON output: $json_delete_output" >&2
    exit 1
fi

output=$(cube workspace stop "Project B")
if [ "$output" != "Workspace Project B stopped" ]; then
    echo "unexpected workspace stop output: $output" >&2
    exit 1
fi

output=$(cube workspace delete "Project B")
if [ "$output" != "Workspace Project B deleted" ]; then
    echo "unexpected workspace delete output: $output" >&2
    exit 1
fi

set +e
cube workspace delete "Project B" >"$tmpdir/delete-missing.out" 2>"$tmpdir/delete-missing.err"
status=$?
set -e
if [ "$status" -ne 1 ]; then
    echo "missing workspace delete should exit 1, got $status" >&2
    exit 1
fi
grep -q 'workspace not found' "$tmpdir/delete-missing.err"

list_output=$(cube workspace list)
if printf "%s\n" "$list_output" | grep -q '	Project B$'; then
    echo "deleted workspace was still listed" >&2
    exit 1
fi

shutdown_response=$(python3 "$CUBICLE_API_CLIENT" "$socket_path" shutdown)
printf "%s" "$shutdown_response" | grep -q '"success": true'
wait "$manager_pid"
manager_pid=
