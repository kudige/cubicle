set -eu

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

state_dir="$tmpdir/state"
stdout_file="$tmpdir/stdout"
stderr_file="$tmpdir/stderr"

"$CUBICLE_CONTROLLER" --state-dir "$state_dir" --mode tty --stdin-policy eof -- \
    sh -c 'test -t 0 && test -t 1 && test -t 2; printf "tty-out\n"; printf "tty-err\n" >&2' \
    >"$stdout_file" 2>"$stderr_file"

grep -q 'tty-out' "$state_dir/stdout.log"
grep -q 'tty-err' "$state_dir/stdout.log"
grep -q 'tty-out' "$stdout_file"
grep -q 'tty-err' "$stdout_file"

if [ -s "$state_dir/stderr.log" ]; then
    echo "plain tty mode should not capture an independent stderr stream" >&2
    exit 1
fi

grep -q '^mode=tty$' "$state_dir/metadata"
grep -q 'seq=1 type=process_started .* mode=tty$' "$state_dir/events.log"
grep -q 'type=output stream=stdout start=0 length=' "$state_dir/events.log"
grep -q 'type=process_exited status=exited exit_code=0$' "$state_dir/events.log"
