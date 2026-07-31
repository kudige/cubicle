set -eu

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

state_dir="$tmpdir/manager"

create_output=$("$CUBICLE_MANAGER" --state-dir "$state_dir" workspace create "Project A")
case "$create_output" in
    workspace\ id=*\ name=Project\ A) ;;
    *)
        echo "unexpected workspace create output: $create_output" >&2
        exit 1
        ;;
esac

workspace_id=${create_output#workspace id=}
workspace_id=${workspace_id%% name=*}

"$CUBICLE_MANAGER" --state-dir "$state_dir" workspace list >"$tmpdir/workspaces"
grep -q "^$workspace_id	Project A$" "$tmpdir/workspaces"
grep -q "^$workspace_id	Project A$" "$state_dir/workspaces.tsv"

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
grep -q "^$process_id	$workspace_id	make-1	stream	running	controller-1	$tmpdir/controller.sock$" "$tmpdir/processes"
grep -q "^$process_id	$workspace_id	make-1	stream	running	controller-1	$tmpdir/controller.sock$" "$state_dir/processes.tsv"

set +e
"$CUBICLE_MANAGER" --state-dir "$state_dir" workspace create "Project A" >/dev/null 2>"$tmpdir/duplicate-error"
duplicate_status=$?
set -e

if [ "$duplicate_status" -eq 0 ]; then
    echo "duplicate workspace creation unexpectedly succeeded" >&2
    exit 1
fi
grep -q 'Workspace already exists: Project A' "$tmpdir/duplicate-error"
