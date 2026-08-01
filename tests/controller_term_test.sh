set -eu

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

state_dir="$tmpdir/state"
stdout_file="$tmpdir/stdout"
stderr_file="$tmpdir/stderr"

"$CUBICLE_CONTROLLER" --state-dir "$state_dir" --mode term --stdin-policy eof -- \
    sh -c 'test -t 0 && test -t 1 && ! test -t 2; printf "term-out\n"; printf "term-err\n" >&2' \
    >"$stdout_file" 2>"$stderr_file"

grep -q 'term-out' "$state_dir/stdout.log"
if grep -q 'term-err' "$state_dir/stdout.log"; then
    echo "term stderr should not be captured in stdout log" >&2
    exit 1
fi

grep -q 'term-err' "$state_dir/stderr.log"
grep -q 'term-out' "$stdout_file"
grep -q 'term-err' "$stderr_file"

grep -q '^mode=term$' "$state_dir/metadata"
grep -q 'seq=1 type=process_started .* mode=term$' "$state_dir/events.log"
grep -q 'type=output stream=stdout start=0 length=' "$state_dir/events.log"
grep -q 'type=output stream=stderr start=0 length=' "$state_dir/events.log"
grep -q 'type=process_exited status=exited exit_code=0$' "$state_dir/events.log"
