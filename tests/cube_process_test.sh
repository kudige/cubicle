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

workspace_output=$("$CUBICLE_MANAGER" --state-dir "$state_dir" workspace create "Project A")
workspace_id=${workspace_output#workspace id=}
workspace_id=${workspace_id%% name=*}

"$CUBICLE_MANAGER" --state-dir "$state_dir" process register \
    --workspace "$workspace_id" \
    --friendly-name build \
    --mode stream \
    --controller-id controller-1 \
    --control-socket "$tmpdir/controller.sock" \
    --process-id process-1 >/dev/null

"$CUBICLE_MANAGER" --state-dir "$state_dir" \
    --controller-bin "$CUBICLE_CONTROLLER" \
    daemon --foreground --control-socket "$socket_path" --event-interval-ms 50 &
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
        CUBICLE_MANAGER_SOCKET="$socket_path" \
        "$CUBE" "$@"
}

ps_output=$(cube --workspace "Project A" ps)
printf "%s\n" "$ps_output" | grep -q '^Workspace Project A$'
printf "%s\n" "$ps_output" | grep -q '^NAME	MODE	STATE$'
printf "%s\n" "$ps_output" | grep -q '^build	stream	lost$'

json_ps_output=$(cube --workspace "Project A" --json ps)
printf "%s" "$json_ps_output" | grep -q '"processes"'
printf "%s" "$json_ps_output" | grep -q '"friendly_name":"build"'
printf "%s" "$json_ps_output" | grep -q '"count":1'

inspect_output=$(cube --workspace "Project A" inspect build)
printf "%s\n" "$inspect_output" | grep -q '^Name:        build$'
printf "%s\n" "$inspect_output" | grep -q '^Workspace:   Project A$'
printf "%s\n" "$inspect_output" | grep -q '^Mode:        stream$'
printf "%s\n" "$inspect_output" | grep -q '^State:       lost$'
printf "%s\n" "$inspect_output" | grep -q '^Process ID:  process-1$'

json_inspect_output=$(cube --workspace "Project A" --json inspect build)
printf "%s" "$json_inspect_output" | grep -q '"id":"process-1"'
printf "%s" "$json_inspect_output" | grep -q '"friendly_name":"build"'

cube workspace select "Project A" >/dev/null
selected_ps_output=$(cube ps)
printf "%s\n" "$selected_ps_output" | grep -q '^build	stream	lost$'

api() {
    python3 "$CUBICLE_API_CLIENT" --raw "$socket_path" "$@"
}

json_id() {
    python3 -c 'import json, sys; print(json.load(sys.stdin)["result"]["id"])'
}

connect_process_id=$(api process-start --workspace "$workspace_id" \
    --friendly-name connect-io -- sh -c \
    'printf "connect-ready\n"; IFS= read line; printf "connect:%s\n" "$line"; sleep 0.2' |
    json_id)
printf "typed\n" | cube connect connect-io \
    >"$tmpdir/connect.out" 2>"$tmpdir/connect.err"
grep -q '^connect-ready$' "$tmpdir/connect.out"
grep -q '^connect:typed$' "$tmpdir/connect.out"
grep -Fq 'Connected to [connect-io]. Detach with Ctrl-\ d' "$tmpdir/connect.err"
api process-wait "$connect_process_id" --timeout-ms 2000 | grep -q '"success":true'
cube remove connect-io >/dev/null

readonly_process_id=$(api process-start --workspace "$workspace_id" \
    --friendly-name connect-ro -- sh -c \
    'printf "readonly-ready\n"; sleep 0.1; printf "readonly-done\n"; sleep 0.2' |
    json_id)
cube connect --ro connect-ro >"$tmpdir/connect-ro.out" 2>"$tmpdir/connect-ro.err"
grep -q '^readonly-ready$' "$tmpdir/connect-ro.out"
grep -q '^readonly-done$' "$tmpdir/connect-ro.out"
grep -Fq 'Connected to [connect-ro]. Detach with Ctrl-\ d' "$tmpdir/connect-ro.err"
api process-wait "$readonly_process_id" --timeout-ms 2000 | grep -q '"success":true'
cube remove connect-ro >/dev/null

if command -v script >/dev/null 2>&1; then
    terminal_lines_process_id=$(api process-start --workspace "$workspace_id" \
        --friendly-name connect-terminal-lines -- sh -c \
        'printf "terminal-line-1\n"; sleep 0.1; printf "terminal-line-2\n"; sleep 0.2' |
        json_id)
    script -qfec "env XDG_STATE_HOME=$xdg_state_home CUBICLE_MANAGER_SOCKET=$socket_path $CUBE connect --ro connect-terminal-lines" \
        /dev/null >"$tmpdir/connect-terminal-lines.out" 2>&1
    python3 - "$tmpdir/connect-terminal-lines.out" <<'PY'
import sys

data = open(sys.argv[1], "rb").read()
if b"terminal-line-1\r\nterminal-line-2" not in data:
    raise SystemExit(f"stream connect output did not preserve terminal line starts: {data!r}")
PY
    api process-wait "$terminal_lines_process_id" --timeout-ms 2000 | grep -q '"success":true'
    cube remove connect-terminal-lines >/dev/null
fi

stream_detach_process_id=$(api process-start --workspace "$workspace_id" \
    --friendly-name connect-stream-detach -- sh -c \
    'printf "stream-detach-ready\n"; sleep 30' |
    json_id)
python3 - "$CUBE" "$socket_path" "$xdg_state_home" "$tmpdir/connect-stream-detach.out" <<'PY'
import os
import pty
import select
import subprocess
import sys
import time

cube, socket_path, xdg_state_home, output_path = sys.argv[1:]
master_fd, slave_fd = pty.openpty()
env = os.environ.copy()
env["CUBICLE_MANAGER_SOCKET"] = socket_path
env["XDG_STATE_HOME"] = xdg_state_home
process = subprocess.Popen(
    [cube, "connect", "connect-stream-detach"],
    stdin=slave_fd,
    stdout=slave_fd,
    stderr=slave_fd,
    env=env,
    close_fds=True,
)
os.close(slave_fd)
captured = bytearray()
try:
    saw_banner = False
    saw_output = False
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        ready, _, _ = select.select([master_fd], [], [], 0.05)
        if ready:
            chunk = os.read(master_fd, 4096)
            captured.extend(chunk)
            saw_banner = saw_banner or b"Connected to [connect-stream-detach]" in captured
            saw_output = saw_output or b"stream-detach-" in captured
            if saw_banner and saw_output:
                break
    else:
        raise AssertionError(f"connect did not attach: {captured!r}")
    os.write(master_fd, b"\x1cd")
    status = process.wait(timeout=5)
    if status != 0:
        raise AssertionError(f"connect detach exited {status}: {captured!r}")
finally:
    with open(output_path, "wb") as handle:
        handle.write(captured)
    if process.poll() is None:
        process.terminate()
        process.wait(timeout=5)
    os.close(master_fd)
PY
grep -q 'stream-detach-' "$tmpdir/connect-stream-detach.out"
cube ps >/dev/null
cube stop connect-stream-detach >/dev/null
api process-wait "$stream_detach_process_id" --timeout-ms 2000 | grep -q '"success":true'
cube remove connect-stream-detach >/dev/null

cube run --stream --name fg-run sh -c \
    'printf "fg-out\n"; printf "fg-err\n" >&2' \
    >"$tmpdir/fg-run.out" 2>"$tmpdir/fg-run.err"
grep -q '^fg-out$' "$tmpdir/fg-run.out"
grep -q '^fg-err$' "$tmpdir/fg-run.err"
cube logs fg-run >"$tmpdir/fg-run-logs.out" 2>"$tmpdir/fg-run-logs.err"
grep -q '^fg-out$' "$tmpdir/fg-run-logs.out"
grep -q '^fg-err$' "$tmpdir/fg-run-logs.err"
for _ in $(seq 1 100); do
    cube events >"$tmpdir/events.out"
    if grep -q 'process_exited' "$tmpdir/events.out"; then
        break
    fi
    sleep 0.05
done
grep -q '^Workspace Project A$' "$tmpdir/events.out"
grep -q '^SEQ	PROCESS	TYPE	PAYLOAD$' "$tmpdir/events.out"
grep -q 'process_started' "$tmpdir/events.out"
grep -q 'output_available' "$tmpdir/events.out"
grep -q 'process_exited' "$tmpdir/events.out"
json_events_output=$(cube --json events)
printf "%s" "$json_events_output" | grep -q '"events"'
printf "%s" "$json_events_output" | grep -q '"count"'
output=$(cube remove fg-run)
if [ "$output" != "Process fg-run removed" ]; then
    echo "unexpected foreground process remove output: $output" >&2
    exit 1
fi

output=$(cube run --bg --stream --name bg-run sleep 30)
if [ "$output" != "[bg-run] started in stream mode" ]; then
    echo "unexpected background run output: $output" >&2
    exit 1
fi
json_bg_output=$(cube --json run --bg --stream /bin/sleep 30)
printf "%s" "$json_bg_output" | grep -q '"friendly_name":"sleep"'
printf "%s" "$json_bg_output" | grep -q '"mode":"stream"'
json_bg_suffix_output=$(cube --json run --bg --stream /bin/sleep 30)
printf "%s" "$json_bg_suffix_output" | grep -q '"friendly_name":"sleep-1"'
printf "%s" "$json_bg_suffix_output" | grep -q '"mode":"stream"'
tty_output=$(cube run --bg --tty --name tty-run sleep 30)
if [ "$tty_output" != "[tty-run] started in tty mode" ]; then
    echo "unexpected tty run output: $tty_output" >&2
    exit 1
fi
cube stop bg-run >/dev/null
cube stop sleep >/dev/null
cube stop sleep-1 >/dev/null
cube stop tty-run >/dev/null

signal_process_id=$(api process-start --workspace "$workspace_id" \
    --friendly-name signal-me sleep 30 | json_id)
output=$(cube signal signal-me TERM)
if [ "$output" != "Process signal-me signaled" ]; then
    echo "unexpected process signal output: $output" >&2
    exit 1
fi
api process-wait "$signal_process_id" --timeout-ms 2000 | grep -q '"success":true'

json_signal_process_id=$(api process-start --workspace "$workspace_id" \
    --friendly-name json-signal-me sleep 30 | json_id)
json_signal_output=$(cube --json signal json-signal-me TERM)
if [ "$json_signal_output" != "{}" ]; then
    echo "unexpected process signal JSON output: $json_signal_output" >&2
    exit 1
fi
api process-wait "$json_signal_process_id" --timeout-ms 2000 | grep -q '"success":true'

stop_process_id=$(api process-start --workspace "$workspace_id" \
    --friendly-name stop-me sleep 30 | json_id)
output=$(cube stop stop-me)
if [ "$output" != "Process stop-me stopped" ]; then
    echo "unexpected process stop output: $output" >&2
    exit 1
fi
api process-wait "$stop_process_id" --timeout-ms 2000 | grep -q '"success":true'

json_stop_process_id=$(api process-start --workspace "$workspace_id" \
    --friendly-name json-stop-me sleep 30 | json_id)
json_stop_output=$(cube --json stop json-stop-me)
if [ "$json_stop_output" != "{}" ]; then
    echo "unexpected process stop JSON output: $json_stop_output" >&2
    exit 1
fi
api process-wait "$json_stop_process_id" --timeout-ms 2000 | grep -q '"success":true'

kill_process_id=$(api process-start --workspace "$workspace_id" \
    --friendly-name kill-me sleep 30 | json_id)
json_kill_output=$(cube --json kill kill-me)
if [ "$json_kill_output" != "{}" ]; then
    echo "unexpected process kill JSON output: $json_kill_output" >&2
    exit 1
fi
api process-wait "$kill_process_id" --timeout-ms 2000 | grep -q '"success":true'

kill_ps_process_id=$(api process-start --workspace "$workspace_id" \
    --friendly-name kill-ps-me sleep 30 | json_id)
cube kill kill-ps-me >/dev/null
for _ in $(seq 1 100); do
    cube ps >"$tmpdir/after-kill-ps.out"
    if grep -q '^kill-ps-me	stream	completed$' "$tmpdir/after-kill-ps.out"; then
        break
    fi
    sleep 0.05
done
grep -q '^kill-ps-me	stream	completed$' "$tmpdir/after-kill-ps.out"
api process-wait "$kill_ps_process_id" --timeout-ms 2000 | grep -q '"success":true'

kill_cleanup_process_id=$(api process-start --workspace "$workspace_id" \
    --friendly-name kill-cleanup-me sleep 30 | json_id)
kill_cleanup_output=$(cube kill --cleanup kill-cleanup-me)
printf "%s\n" "$kill_cleanup_output" | grep -q '^Process kill-cleanup-me killed$'
printf "%s\n" "$kill_cleanup_output" | grep -q '^Process kill-cleanup-me removed$'
set +e
api process-get "$kill_cleanup_process_id" >"$tmpdir/kill-cleanup-get.out" 2>&1
status=$?
set -e
if [ "$status" -eq 0 ]; then
    echo "kill --cleanup should remove killed process" >&2
    exit 1
fi

kill_all_one_process_id=$(api process-start --workspace "$workspace_id" \
    --friendly-name kill-all-one sleep 30 | json_id)
kill_all_two_process_id=$(api process-start --workspace "$workspace_id" \
    --friendly-name kill-all-two sleep 30 | json_id)
kill_all_output=$(cube kill --all --cleanup)
printf "%s\n" "$kill_all_output" | grep -q '^Killed 2 processes$'
printf "%s\n" "$kill_all_output" | grep -q '^Removed 2 processes$'
set +e
api process-get "$kill_all_one_process_id" >"$tmpdir/kill-all-one-get.out" 2>&1
first_status=$?
api process-get "$kill_all_two_process_id" >"$tmpdir/kill-all-two-get.out" 2>&1
second_status=$?
set -e
if [ "$first_status" -eq 0 ] || [ "$second_status" -eq 0 ]; then
    echo "kill --all --cleanup should remove killed processes" >&2
    exit 1
fi

kill_config_process_id=$(api process-start --workspace "$workspace_id" \
    --friendly-name kill-config-cleanup sleep 30 | json_id)
config_path="$tmpdir/kill-cleanup.cfg"
cat >"$config_path" <<EOF
[client]
manager=unix://$socket_path

[defaults]
kill_cleanup=true
EOF
kill_config_output=$(XDG_STATE_HOME="$xdg_state_home" CUBICLE_CONFIG="$config_path" "$CUBE" kill kill-config-cleanup)
printf "%s\n" "$kill_config_output" | grep -q '^Process kill-config-cleanup killed$'
printf "%s\n" "$kill_config_output" | grep -q '^Process kill-config-cleanup removed$'
set +e
api process-get "$kill_config_process_id" >"$tmpdir/kill-config-get.out" 2>&1
status=$?
set -e
if [ "$status" -eq 0 ]; then
    echo "configured kill cleanup should remove killed process" >&2
    exit 1
fi

remove_process_id=$(api process-start --workspace "$workspace_id" \
    --friendly-name remove-me /bin/true | json_id)
api process-wait "$remove_process_id" --timeout-ms 2000 | grep -q '"success":true'
output=$(cube remove remove-me)
if [ "$output" != "Process remove-me removed" ]; then
    echo "unexpected process remove output: $output" >&2
    exit 1
fi
json_remove_process_id=$(api process-start --workspace "$workspace_id" \
    --friendly-name json-remove-me /bin/true | json_id)
api process-wait "$json_remove_process_id" --timeout-ms 2000 | grep -q '"success":true'
json_remove_output=$(cube --json remove json-remove-me)
if [ "$json_remove_output" != "{}" ]; then
    echo "unexpected process remove JSON output: $json_remove_output" >&2
    exit 1
fi

cleanup_workspace_id=$(api workspace-create "Cleanup Workspace" | json_id)
cleanup_done_process_id=$(api process-start --workspace "$cleanup_workspace_id" \
    --friendly-name cleanup-done /bin/true | json_id)
api process-wait "$cleanup_done_process_id" --timeout-ms 2000 | grep -q '"success":true'
cleanup_live_process_id=$(api process-start --workspace "$cleanup_workspace_id" \
    --friendly-name cleanup-live sleep 30 | json_id)
cleanup_output=$(cube --workspace "Cleanup Workspace" cleanup)
printf "%s\n" "$cleanup_output" | grep -q '^Removed 1 processes$'
printf "%s\n" "$cleanup_output" | grep -q '^Skipped 1 live processes$'
set +e
cube --workspace "Cleanup Workspace" inspect cleanup-done \
    >"$tmpdir/cleanup-done.out" 2>"$tmpdir/cleanup-done.err"
status=$?
set -e
if [ "$status" -ne 1 ]; then
    echo "cleaned process inspect should exit 1, got $status" >&2
    exit 1
fi
grep -q 'process not found' "$tmpdir/cleanup-done.err"
cube --workspace "Cleanup Workspace" inspect cleanup-live \
    >"$tmpdir/cleanup-live.out"
grep -q '^State:       running$' "$tmpdir/cleanup-live.out"
json_cleanup_output=$(cube --workspace "Cleanup Workspace" --json cleanup)
printf "%s" "$json_cleanup_output" | grep -q '"removed_count":0'
printf "%s" "$json_cleanup_output" | grep -q '"skipped_live_count":1'
api process-kill "$cleanup_live_process_id" >/dev/null
api process-wait "$cleanup_live_process_id" --timeout-ms 2000 | grep -q '"success":true'
cube --workspace "Cleanup Workspace" cleanup >/dev/null
cube workspace delete "Cleanup Workspace" >/dev/null

set +e
cube inspect remove-me >"$tmpdir/removed-inspect.out" 2>"$tmpdir/removed-inspect.err"
status=$?
set -e
if [ "$status" -ne 1 ]; then
    echo "removed process inspect should exit 1, got $status" >&2
    exit 1
fi
grep -q 'process not found' "$tmpdir/removed-inspect.err"

set +e
XDG_STATE_HOME="$tmpdir/empty-state" CUBICLE_MANAGER_SOCKET="$socket_path" \
    "$CUBE" ps >"$tmpdir/no-workspace.out" 2>"$tmpdir/no-workspace.err"
status=$?
set -e
if [ "$status" -ne 1 ]; then
    echo "cube ps without selected workspace should exit 1, got $status" >&2
    exit 1
fi
grep -q 'no workspace selected' "$tmpdir/no-workspace.err"

set +e
cube inspect >"$tmpdir/inspect-missing.out" 2>"$tmpdir/inspect-missing.err"
status=$?
set -e
if [ "$status" -ne 2 ]; then
    echo "cube inspect without process should exit 2, got $status" >&2
    exit 1
fi
grep -q 'inspect requires a process name' "$tmpdir/inspect-missing.err"

set +e
cube signal build NOPE >"$tmpdir/signal-invalid.out" 2>"$tmpdir/signal-invalid.err"
status=$?
set -e
if [ "$status" -ne 2 ]; then
    echo "cube signal with invalid signal should exit 2, got $status" >&2
    exit 1
fi
grep -q 'invalid signal' "$tmpdir/signal-invalid.err"

cube run --fg --term --name term-run sh -c \
    'test -t 0 && test -t 1 && test -t 2; printf "term-out\n"; printf "term-err\n" >&2; sleep 0.2' \
    >"$tmpdir/term-run.out" 2>"$tmpdir/term-run.err"
grep -q 'term-out' "$tmpdir/term-run.out"
grep -q 'term-err' "$tmpdir/term-run.err"
if grep -q 'term-err' "$tmpdir/term-run.out"; then
    echo "cube foreground term stderr should not be merged into stdout" >&2
    exit 1
fi
cube logs term-run >"$tmpdir/term-run-logs.out" 2>"$tmpdir/term-run-logs.err"
grep -q 'term-out' "$tmpdir/term-run-logs.out"
grep -q 'term-err' "$tmpdir/term-run-logs.err"
if grep -q 'term-err' "$tmpdir/term-run-logs.out"; then
    echo "cube term logs should keep stderr separate" >&2
    exit 1
fi
cube remove term-run >/dev/null

follow_logs_process_id=$(api process-start --workspace "$workspace_id" \
    --friendly-name follow-logs -- sh -c \
    'printf "follow-out-1\n"; printf "follow-err-1\n" >&2; sleep 0.2; printf "follow-out-2\n"; printf "follow-err-2\n" >&2' |
    json_id)
cube logs --follow follow-logs \
    >"$tmpdir/logs-follow.out" 2>"$tmpdir/logs-follow.err"
grep -q '^follow-out-1$' "$tmpdir/logs-follow.out"
grep -q '^follow-out-2$' "$tmpdir/logs-follow.out"
grep -q '^follow-err-1$' "$tmpdir/logs-follow.err"
grep -q '^follow-err-2$' "$tmpdir/logs-follow.err"
api process-wait "$follow_logs_process_id" --timeout-ms 2000 | grep -q '"success":true'
cube remove follow-logs >/dev/null

follow_workspace_id=$(api workspace-create "Follow Workspace" | json_id)
cube --workspace "Follow Workspace" events --follow --iterations 200 \
    >"$tmpdir/events-follow.out" 2>"$tmpdir/events-follow.err" &
events_follow_pid=$!
for _ in $(seq 1 100); do
    if grep -q '^SEQ	PROCESS	TYPE	PAYLOAD$' "$tmpdir/events-follow.out"; then
        break
    fi
    sleep 0.05
done
follow_events_process_id=$(api process-start --workspace "$follow_workspace_id" \
    --friendly-name follow-events /bin/true | json_id)
api process-wait "$follow_events_process_id" --timeout-ms 2000 | grep -q '"success":true'
wait "$events_follow_pid"
if ! grep -q "$follow_events_process_id" "$tmpdir/events-follow.out" ||
    ! grep -q 'process_exited' "$tmpdir/events-follow.out"; then
    echo "cube events --follow did not observe the test process" >&2
    cat "$tmpdir/events-follow.out" >&2
    exit 1
fi
cube --workspace "Follow Workspace" remove follow-events >/dev/null
cube workspace delete "Follow Workspace" >/dev/null

set +e
cube run >"$tmpdir/run-missing.out" 2>"$tmpdir/run-missing.err"
status=$?
set -e
if [ "$status" -ne 2 ]; then
    echo "cube run without command should exit 2, got $status" >&2
    exit 1
fi
grep -q 'run requires a command' "$tmpdir/run-missing.err"

shutdown_response=$(python3 "$CUBICLE_API_CLIENT" "$socket_path" shutdown)
printf "%s" "$shutdown_response" | grep -q '"success": true'
wait "$manager_pid"
manager_pid=
