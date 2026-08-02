set -eu

rm -f "$PWD/controller-help.out" "$PWD/controller-help.err"
rm -f "$PWD/manager-help.out" "$PWD/manager-help.err"
rm -f "$PWD/cube-help.out" "$PWD/cube-help.err"
rm -f "$PWD/cube-missing-manager.out" "$PWD/cube-missing-manager.err"
rm -f "$PWD/cube-unimplemented.out" "$PWD/cube-unimplemented.err"
rm -f "$PWD/cube-unknown.out" "$PWD/cube-unknown.err"

"$CUBICLE_CONTROLLER" --help >"$PWD/controller-help.out" 2>"$PWD/controller-help.err"
if [ -s "$PWD/controller-help.out" ]; then
    echo "controller help should write to stderr only" >&2
    exit 1
fi
grep -q 'Usage: .*cubicle-controller' "$PWD/controller-help.err"
grep -q -- '--completed-retention-ms N' "$PWD/controller-help.err"
grep -q -- '--control-socket path' "$PWD/controller-help.err"

"$CUBICLE_MANAGER" --help >"$PWD/manager-help.out" 2>"$PWD/manager-help.err"
if [ -s "$PWD/manager-help.out" ]; then
    echo "manager help should write to stderr only" >&2
    exit 1
fi
grep -q 'Usage: .*cubicle-manager' "$PWD/manager-help.err"
grep -q 'events follow \[--iterations N\]' "$PWD/manager-help.err"
grep -q 'daemon \[--foreground\] \[--control-socket PATH\] \[--listen URI\] \[--allow-insecure\] \[--event-interval-ms N\]' "$PWD/manager-help.err"

"$CUBE" --help >"$PWD/cube-help.out" 2>"$PWD/cube-help.err"
if [ -s "$PWD/cube-help.err" ]; then
    echo "cube help should write to stdout only" >&2
    exit 1
fi
grep -q 'Usage:' "$PWD/cube-help.out"
grep -q 'cube workspace NAME' "$PWD/cube-help.out"
grep -q 'cube run \[--fg|--bg\] \[--stream|--tty|--term\]' "$PWD/cube-help.out"
grep -q 'cube logs \[--follow\] NAME' "$PWD/cube-help.out"
grep -q 'cube events \[--follow \[--iterations N\]\]' "$PWD/cube-help.out"
grep -q 'cube config show|paths|validate' "$PWD/cube-help.out"
grep -q 'cube cleanup' "$PWD/cube-help.out"
grep -q 'cube access list|add|set-role|remove' "$PWD/cube-help.out"
grep -q 'cube connect \[--ro\] NAME' "$PWD/cube-help.out"

set +e
env -u CUBICLE_MANAGER_SOCKET "$CUBE" ps \
    >"$PWD/cube-missing-manager.out" 2>"$PWD/cube-missing-manager.err"
status=$?
set -e
if [ "$status" -ne 2 ]; then
    echo "cube missing manager should exit 2, got $status" >&2
    exit 1
fi
if [ -s "$PWD/cube-missing-manager.out" ]; then
    echo "cube missing manager should not write stdout" >&2
    exit 1
fi
grep -q 'failed to connect to manager' "$PWD/cube-missing-manager.err"

set +e
"$CUBE" --manager-socket /tmp/cubicle-test.sock defaults >"$PWD/cube-unimplemented.out" 2>"$PWD/cube-unimplemented.err"
status=$?
set -e
if [ "$status" -ne 2 ]; then
    echo "cube unimplemented command should exit 2, got $status" >&2
    exit 1
fi
grep -q "command 'defaults' is not implemented yet" "$PWD/cube-unimplemented.err"

set +e
"$CUBE" wat >"$PWD/cube-unknown.out" 2>"$PWD/cube-unknown.err"
status=$?
set -e
if [ "$status" -ne 2 ]; then
    echo "cube unknown command should exit 2, got $status" >&2
    exit 1
fi
grep -q "unknown command 'wat'" "$PWD/cube-unknown.err"

rm -f "$PWD/controller-help.out" "$PWD/controller-help.err"
rm -f "$PWD/manager-help.out" "$PWD/manager-help.err"
rm -f "$PWD/cube-help.out" "$PWD/cube-help.err"
rm -f "$PWD/cube-missing-manager.out" "$PWD/cube-missing-manager.err"
rm -f "$PWD/cube-unimplemented.out" "$PWD/cube-unimplemented.err"
rm -f "$PWD/cube-unknown.out" "$PWD/cube-unknown.err"
