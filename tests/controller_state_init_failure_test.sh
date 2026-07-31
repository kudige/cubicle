set -eu

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

state_path="$tmpdir/not-a-directory"
touch "$state_path"

set +e
"$CUBICLE_CONTROLLER" \
    --state-dir "$state_path" \
    --mode stream \
    -- sh -c 'sleep 30' \
    >/dev/null 2>"$tmpdir/stderr"
status=$?
set -e

if [ "$status" -ne 1 ]; then
    echo "expected controller status 1 for invalid state dir, got $status" >&2
    exit 1
fi

grep -q "failed to initialize state $state_path:" "$tmpdir/stderr"

if [ -d "$state_path" ]; then
    echo "invalid state path was converted into a directory" >&2
    exit 1
fi
