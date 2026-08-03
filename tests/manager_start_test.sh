set -eu

tmpdir=$(mktemp -d)
trap 'if [ -n "${control_socket:-}" ] && [ -S "$control_socket" ]; then python3 "$CUBICLE_CONTROL_CLIENT" "$control_socket" terminate >/dev/null 2>&1 || true; fi; rm -rf "$tmpdir"' EXIT

state_dir="$tmpdir/manager"

wait_for_resolve_match() {
    process_id=$1
    output_path=$2
    expected_pattern=$3

    for _ in $(seq 1 100); do
        "$CUBICLE_MANAGER" --state-dir "$state_dir" events poll >/dev/null
        "$CUBICLE_MANAGER" --state-dir "$state_dir" process resolve "$process_id" >"$output_path"
        if grep -Eq "$expected_pattern" "$output_path"; then
            return 0
        fi
        sleep 0.05
    done

    grep -Eq "$expected_pattern" "$output_path"
}

workspace_output=$("$CUBICLE_MANAGER" --state-dir "$state_dir" workspace create "Project A")
workspace_id=${workspace_output#workspace id=}
workspace_id=${workspace_id%% name=*}
workspace_dir=${workspace_output##* directory=}

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
grep -q "^$process_id	$workspace_id	started-1	stream	running	$controller_id	$control_socket	$workspace_dir$" "$tmpdir/processes"

"$CUBICLE_MANAGER" --state-dir "$state_dir" process resolve started-1 --workspace "Project A" >"$tmpdir/resolve-started"
grep -q "^$process_id	$workspace_id	started-1	stream	running	$controller_id	$control_socket	$workspace_dir$" "$tmpdir/resolve-started"

if "$CUBICLE_MANAGER" \
    --state-dir "$state_dir" \
    --controller-bin "$CUBICLE_CONTROLLER" \
    process start \
    --workspace "Project A" \
    --friendly-name started-1 \
    --mode stream \
    --stdin-policy eof \
    -- sh -c 'printf "duplicate-start\n"' >/dev/null 2>"$tmpdir/duplicate-start-error"; then
    echo "duplicate process start friendly name unexpectedly succeeded" >&2
    exit 1
fi
grep -q 'Process friendly name already exists in workspace: started-1' "$tmpdir/duplicate-start-error"

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

fast_output=$("$CUBICLE_MANAGER" \
    --state-dir "$state_dir" \
    --controller-bin "$CUBICLE_CONTROLLER" \
    process start \
    --workspace "Project A" \
    --friendly-name fast-1 \
    --mode stream \
    --stdin-policy eof \
    -- sh -c 'printf "fast\n"')

fast_process_id=${fast_output#process id=}
fast_process_id=${fast_process_id%% workspace_id=*}
fast_socket=${fast_output##* control_socket=}

wait_for_resolve_match "$fast_process_id" "$tmpdir/resolve-fast" \
    "^$fast_process_id	$workspace_id	fast-1	stream	(exited|completed)	.*	$fast_socket	$workspace_dir$"

if [ -S "$fast_socket" ]; then
    echo "fast process should not leave a live control socket" >&2
    exit 1
fi

printf "fast\n" | cmp - "$state_dir/controllers/$fast_process_id/stdout.log"

tty_output=$("$CUBICLE_MANAGER" \
    --state-dir "$state_dir" \
    --controller-bin "$CUBICLE_CONTROLLER" \
    process start \
    --workspace "Project A" \
    --friendly-name tty-1 \
    --mode tty \
    --stdin-policy eof \
    -- sh -c 'test -t 0 && test -t 1 && test -t 2; printf "manager-tty\n"; printf "manager-tty-err\n" >&2')

tty_process_id=${tty_output#process id=}
tty_process_id=${tty_process_id%% workspace_id=*}
tty_socket=${tty_output##* control_socket=}

wait_for_resolve_match "$tty_process_id" "$tmpdir/resolve-tty" \
    "^$tty_process_id	$workspace_id	tty-1	tty	(exited|completed)	.*	$tty_socket	$workspace_dir$"

grep -q 'manager-tty' "$state_dir/controllers/$tty_process_id/stdout.log"
grep -q 'manager-tty-err' "$state_dir/controllers/$tty_process_id/stdout.log"

if [ -s "$state_dir/controllers/$tty_process_id/stderr.log" ]; then
    echo "manager-started tty should not capture independent stderr" >&2
    exit 1
fi

term_output=$("$CUBICLE_MANAGER" \
    --state-dir "$state_dir" \
    --controller-bin "$CUBICLE_CONTROLLER" \
    process start \
    --workspace "Project A" \
    --friendly-name term-1 \
    --mode term \
    --stdin-policy eof \
    -- sh -c 'test -t 0 && test -t 1 && test -t 2 && printf "manager-term\n"; printf "manager-term-err\n" >&2')

term_process_id=${term_output#process id=}
term_process_id=${term_process_id%% workspace_id=*}
term_socket=${term_output##* control_socket=}

wait_for_resolve_match "$term_process_id" "$tmpdir/resolve-term" \
    "^$term_process_id	$workspace_id	term-1	term	(exited|completed)	.*	$term_socket	$workspace_dir$"

grep -q 'manager-term' "$state_dir/controllers/$term_process_id/stdout.log"
if grep -q 'manager-term-err' "$state_dir/controllers/$term_process_id/stdout.log"; then
    echo "manager-started term stderr should not be captured in stdout" >&2
    exit 1
fi
grep -q 'manager-term-err' "$state_dir/controllers/$term_process_id/stderr.log"
