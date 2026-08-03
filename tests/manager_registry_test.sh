set -eu

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

state_dir="$tmpdir/manager"

create_output=$("$CUBICLE_MANAGER" --state-dir "$state_dir" workspace create "Project A")
case "$create_output" in
    workspace\ id=*\ name=Project\ A\ directory=*) ;;
    *)
        echo "unexpected workspace create output: $create_output" >&2
        exit 1
        ;;
esac

workspace_id=${create_output#workspace id=}
workspace_id=${workspace_id%% name=*}
workspace_dir=${create_output##* directory=}

"$CUBICLE_MANAGER" --state-dir "$state_dir" workspace list >"$tmpdir/workspaces"
grep -q "^$workspace_id	Project A	$workspace_dir$" "$tmpdir/workspaces"
grep -q "^$workspace_id	Project A	$workspace_dir$" "$state_dir/workspaces.tsv"

register_output=$("$CUBICLE_MANAGER" --state-dir "$state_dir" process register \
    --workspace "Project A" \
    --friendly-name make-1 \
    --mode stream \
    --controller-id controller-1 \
    --control-socket "$tmpdir/controller.sock")

case "$register_output" in
    process\ id=*\ workspace_id="$workspace_id"\ friendly_name=make-1\ controller_id=controller-1\ control_socket="$tmpdir/controller.sock") ;;
    *)
        echo "unexpected process register output: $register_output" >&2
        exit 1
        ;;
esac

process_id=${register_output#process id=}
process_id=${process_id%% workspace_id=*}

"$CUBICLE_MANAGER" --state-dir "$state_dir" process list --workspace "Project A" >"$tmpdir/processes"
grep -q "^$process_id	$workspace_id	make-1	stream	running	controller-1	$tmpdir/controller.sock	$workspace_dir	0$" "$tmpdir/processes"
grep -q "^$process_id	$workspace_id	make-1	stream	running	controller-1	$tmpdir/controller.sock	$workspace_dir	0$" "$state_dir/processes.tsv"

"$CUBICLE_MANAGER" --state-dir "$state_dir" process resolve "$process_id" >"$tmpdir/resolve-by-id"
grep -q "^$process_id	$workspace_id	make-1	stream	running	controller-1	$tmpdir/controller.sock	$workspace_dir	0$" "$tmpdir/resolve-by-id"

"$CUBICLE_MANAGER" --state-dir "$state_dir" process resolve make-1 --workspace "Project A" >"$tmpdir/resolve-by-name"
grep -q "^$process_id	$workspace_id	make-1	stream	running	controller-1	$tmpdir/controller.sock	$workspace_dir	0$" "$tmpdir/resolve-by-name"

if "$CUBICLE_MANAGER" --state-dir "$state_dir" process register \
    --workspace "Project A" \
    --friendly-name make-2 \
    --mode stream \
    --controller-id controller-duplicate-id \
    --control-socket "$tmpdir/controller-duplicate-id.sock" \
    --process-id "$process_id" >/dev/null 2>"$tmpdir/duplicate-process-id-error"; then
    echo "duplicate process id registration unexpectedly succeeded" >&2
    exit 1
fi
grep -q "Process already exists: $process_id" "$tmpdir/duplicate-process-id-error"

if "$CUBICLE_MANAGER" --state-dir "$state_dir" process register \
    --workspace "Project A" \
    --friendly-name make-1 \
    --mode stream \
    --controller-id controller-duplicate-name \
    --control-socket "$tmpdir/controller-duplicate-name.sock" >/dev/null 2>"$tmpdir/duplicate-friendly-name-error"; then
    echo "duplicate friendly-name registration unexpectedly succeeded" >&2
    exit 1
fi
grep -q 'Process friendly name already exists in workspace: make-1' "$tmpdir/duplicate-friendly-name-error"

workspace_b_output=$("$CUBICLE_MANAGER" --state-dir "$state_dir" workspace create "Project B")
workspace_b_id=${workspace_b_output#workspace id=}
workspace_b_id=${workspace_b_id%% name=*}
workspace_b_dir=${workspace_b_output##* directory=}

register_b_output=$("$CUBICLE_MANAGER" --state-dir "$state_dir" process register \
    --workspace "Project B" \
    --friendly-name make-1 \
    --mode stream \
    --controller-id controller-b \
    --control-socket "$tmpdir/controller-b.sock")

process_b_id=${register_b_output#process id=}
process_b_id=${process_b_id%% workspace_id=*}

"$CUBICLE_MANAGER" --state-dir "$state_dir" process resolve make-1 --workspace "Project B" >"$tmpdir/resolve-by-name-b"
grep -q "^$process_b_id	$workspace_b_id	make-1	stream	running	controller-b	$tmpdir/controller-b.sock	$workspace_b_dir	0$" "$tmpdir/resolve-by-name-b"

set +e
"$CUBICLE_MANAGER" --state-dir "$state_dir" process resolve make-1 >/dev/null 2>"$tmpdir/resolve-error"
resolve_status=$?
set -e

if [ "$resolve_status" -eq 0 ]; then
    echo "friendly-name resolution without workspace unexpectedly succeeded" >&2
    exit 1
fi
grep -q 'friendly names require --workspace' "$tmpdir/resolve-error"

set +e
"$CUBICLE_MANAGER" --state-dir "$state_dir" workspace create "Project A" >/dev/null 2>"$tmpdir/duplicate-error"
duplicate_status=$?
set -e

if [ "$duplicate_status" -eq 0 ]; then
    echo "duplicate workspace creation unexpectedly succeeded" >&2
    exit 1
fi
grep -q 'Workspace already exists: Project A' "$tmpdir/duplicate-error"
