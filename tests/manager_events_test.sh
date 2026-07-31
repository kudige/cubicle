set -eu

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

state_dir="$tmpdir/manager"

workspace_output=$("$CUBICLE_MANAGER" --state-dir "$state_dir" workspace create "Project A")
workspace_id=${workspace_output#workspace id=}
workspace_id=${workspace_id%% name=*}

start_output=$("$CUBICLE_MANAGER" \
    --state-dir "$state_dir" \
    --controller-bin "$CUBICLE_CONTROLLER" \
    process start \
    --workspace "Project A" \
    --friendly-name events-1 \
    --mode stream \
    --stdin-policy eof \
    -- sh -c 'printf "event-output\n"; sleep 0.5')

process_id=${start_output#process id=}
process_id=${process_id%% workspace_id=*}

for _ in $(seq 1 100); do
    controller_events="$state_dir/controllers/$process_id/events.log"
    if [ -f "$controller_events" ] && grep -q 'type=process_exited' "$controller_events"; then
        break
    fi
    sleep 0.05
done

"$CUBICLE_MANAGER" --state-dir "$state_dir" events poll --workspace "Project A" >"$tmpdir/events-1"

grep -q "^$workspace_id	$process_id	events-1	seq=1 type=process_started" "$tmpdir/events-1"
grep -q "^$workspace_id	$process_id	events-1	seq=.*type=output stream=stdout" "$tmpdir/events-1"
grep -q "^$workspace_id	$process_id	events-1	seq=.*type=process_exited status=exited exit_code=0" "$tmpdir/events-1"

cmp "$tmpdir/events-1" "$state_dir/workspace-events.log"
grep -q "^$process_id	" "$state_dir/cursors.tsv"

"$CUBICLE_MANAGER" --state-dir "$state_dir" events list --workspace "Project A" >"$tmpdir/events-list"
cmp "$tmpdir/events-1" "$tmpdir/events-list"

"$CUBICLE_MANAGER" --state-dir "$state_dir" events poll --workspace "$workspace_id" >"$tmpdir/events-2"
if [ -s "$tmpdir/events-2" ]; then
    echo "second events poll should not emit already-consumed events" >&2
    exit 1
fi

"$CUBICLE_MANAGER" --state-dir "$state_dir" events follow --workspace "$workspace_id" --iterations 1 --interval-ms 0 >"$tmpdir/events-follow"
if [ -s "$tmpdir/events-follow" ]; then
    echo "follow should not emit already-consumed events" >&2
    exit 1
fi

if "$CUBICLE_MANAGER" --state-dir "$state_dir" events follow --workspace "$workspace_id" --iterations -1 --interval-ms 0 >"$tmpdir/events-invalid.out" 2>"$tmpdir/events-invalid.err"; then
    echo "negative follow iterations should fail" >&2
    exit 1
fi
grep -q 'events follow requires nonnegative --iterations and --interval-ms' "$tmpdir/events-invalid.err"

follow_output="$tmpdir/events-follow-live"
"$CUBICLE_MANAGER" --state-dir "$state_dir" events follow --workspace "$workspace_id" --interval-ms 50 >"$follow_output" &
follow_pid=$!
trap 'if [ -n "${follow_pid:-}" ]; then kill "$follow_pid" 2>/dev/null || true; wait "$follow_pid" 2>/dev/null || true; fi; rm -rf "$tmpdir"' EXIT

follow_start_output=$("$CUBICLE_MANAGER" \
    --state-dir "$state_dir" \
    --controller-bin "$CUBICLE_CONTROLLER" \
    process start \
    --workspace "Project A" \
    --friendly-name events-follow-1 \
    --mode stream \
    --stdin-policy eof \
    -- sh -c 'printf "follow-output\n"')

follow_process_id=${follow_start_output#process id=}
follow_process_id=${follow_process_id%% workspace_id=*}

for _ in $(seq 1 100); do
    if grep -q "^$workspace_id	$follow_process_id	events-follow-1	seq=.*type=process_exited status=exited exit_code=0" "$follow_output"; then
        break
    fi
    sleep 0.05
done

kill "$follow_pid" 2>/dev/null || true
wait "$follow_pid" 2>/dev/null || true
follow_pid=

grep -q "^$workspace_id	$follow_process_id	events-follow-1	seq=1 type=process_started" "$follow_output"
grep -q "^$workspace_id	$follow_process_id	events-follow-1	seq=.*type=output stream=stdout" "$follow_output"
grep -q "^$workspace_id	$follow_process_id	events-follow-1	seq=.*type=process_exited status=exited exit_code=0" "$follow_output"
