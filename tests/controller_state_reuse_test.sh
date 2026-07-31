set -eu

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

state_dir="$tmpdir/state"
mkdir "$state_dir"
printf "keep-me\n" >"$state_dir/stdout.log"

set +e
"$CUBICLE_CONTROLLER" \
    --state-dir "$state_dir" \
    --mode stream \
    -- sh -c 'sleep 30' \
    >/dev/null 2>"$tmpdir/stderr"
status=$?
set -e

if [ "$status" -ne 1 ]; then
    echo "expected controller status 1 for reused state dir, got $status" >&2
    exit 1
fi

grep -q 'failed to initialize state:' "$tmpdir/stderr"
printf "keep-me\n" | cmp - "$state_dir/stdout.log"
