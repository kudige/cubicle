set -eu

tmpdir=$(mktemp -d)
trap 'if [ -n "${control_socket:-}" ] && [ -S "$control_socket" ]; then python3 "$CUBICLE_CONTROL_CLIENT" "$control_socket" terminate >/dev/null 2>&1 || true; fi; rm -rf "$tmpdir"' EXIT

state_dir="$tmpdir/manager"

workspace_output=$("$CUBICLE_MANAGER" --state-dir "$state_dir" workspace create "Project A")
workspace_id=${workspace_output#workspace id=}
workspace_id=${workspace_id%% name=*}

start_output=$("$CUBICLE_MANAGER" \
    --state-dir "$state_dir" \
    --controller-bin "$CUBICLE_CONTROLLER" \
    process start \
    --workspace "Project A" \
    --friendly-name started-1 \
    --mode stream \
    --stdin-policy eof \
    -- sh -c 'printf "manager-start\n"; sleep 30')

case "$start_output" in
    process\ id=*\ workspace_id="$workspace_id"\ friendly_name=started-1\ controller_id=*\ control_socket=*) ;;
    *)
        echo "unexpected process start output: $start_output" >&2
        exit 1
        ;;
esac

process_id=${start_output#process id=}
process_id=${process_id%% workspace_id=*}
control_socket=${start_output##* control_socket=}
controller_id_part=${start_output#* controller_id=}
controller_id=${controller_id_part%% control_socket=*}

if [ ! -S "$control_socket" ]; then
    echo "manager did not return a live control socket" >&2
    exit 1
fi

"$CUBICLE_MANAGER" --state-dir "$state_dir" process list --workspace "$workspace_id" >"$tmpdir/processes"
grep -q "^$process_id	$workspace_id	started-1	stream	running	$controller_id	$control_socket$" "$tmpdir/processes"

for _ in $(seq 1 100); do
    read_response=$(python3 "$CUBICLE_CONTROL_CLIENT" "$control_socket" read stdout 0 14)
    if [ "$read_response" = "$(printf 'ok length=14\nmanager-start\n')" ]; then
        break
    fi
    sleep 0.05
done

if [ "$read_response" != "$(printf 'ok length=14\nmanager-start\n')" ]; then
    echo "unexpected read response from manager-started controller: $read_response" >&2
    exit 1
fi

metadata_response=$(python3 "$CUBICLE_CONTROL_CLIENT" "$control_socket" metadata)
printf "%s\n" "$metadata_response" | grep -q "^controller_id=$controller_id$"
printf "%s\n" "$metadata_response" | grep -q '^stdin_policy=eof$'

terminate_response=$(python3 "$CUBICLE_CONTROL_CLIENT" "$control_socket" terminate)
if [ "$terminate_response" != "ok" ]; then
    echo "unexpected terminate response: $terminate_response" >&2
    exit 1
fi

for _ in $(seq 1 100); do
    if [ ! -S "$control_socket" ]; then
        break
    fi
    sleep 0.05
done

if [ -S "$control_socket" ]; then
    echo "manager-started controller did not stop" >&2
    exit 1
fi
