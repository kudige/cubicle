set -eu

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

empty_state_dir="$tmpdir/empty-state"
mkdir "$empty_state_dir"

"$CUBICLE_CONTROLLER" \
    --state-dir "$empty_state_dir" \
    --mode stream \
    --stdin-policy eof \
    -- sh -c 'printf "empty-dir-ok\n"' \
    >/dev/null 2>"$tmpdir/empty-stderr"

printf "empty-dir-ok\n" | cmp - "$empty_state_dir/stdout.log"

reused_state_dir="$tmpdir/reused-state"
mkdir "$reused_state_dir"
printf "keep-me\n" >"$reused_state_dir/stdout.log"

set +e
"$CUBICLE_CONTROLLER" \
    --state-dir "$reused_state_dir" \
    --mode stream \
    -- sh -c 'sleep 30' \
    >/dev/null 2>"$tmpdir/stderr"
status=$?
set -e

if [ "$status" -ne 1 ]; then
    echo "expected controller status 1 for reused state dir, got $status" >&2
    exit 1
fi

grep -q "failed to initialize state $reused_state_dir: File exists" "$tmpdir/stderr"
printf "keep-me\n" | cmp - "$reused_state_dir/stdout.log"
