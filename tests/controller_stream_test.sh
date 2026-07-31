set -eu

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

state_dir="$tmpdir/state"
stdout_file="$tmpdir/stdout"
stderr_file="$tmpdir/stderr"

set +e
"$CUBICLE_CONTROLLER" --state-dir "$state_dir" --mode stream -- sh -c 'printf "out\n"; printf "err\n" >&2; exit 7' >"$stdout_file" 2>"$stderr_file"
status=$?
set -e

if [ "$status" -ne 7 ]; then
    echo "expected controller to return child exit status 7, got $status" >&2
    exit 1
fi

cmp "$stdout_file" "$state_dir/stdout.log"
printf "out\n" | cmp - "$state_dir/stdout.log"
printf "err\n" | cmp - "$state_dir/stderr.log"

grep -q '^mode=stream$' "$state_dir/metadata"
grep -q '^command=sh -c printf "out\\n"; printf "err\\n" >&2; exit 7$' "$state_dir/metadata"
grep -q 'seq=1 type=process_started .* mode=stream$' "$state_dir/events.log"
grep -q 'type=output stream=stdout start=0 length=4$' "$state_dir/events.log"
grep -q 'type=output stream=stderr start=0 length=4$' "$state_dir/events.log"
grep -q 'type=process_exited status=exited exit_code=7$' "$state_dir/events.log"
