set -eu

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT
mkdir -p "$tmpdir/xdg-runtime"

cube() {
    XDG_STATE_HOME="$tmpdir/xdg-state" \
        XDG_CONFIG_HOME="$tmpdir/xdg-config" \
        XDG_RUNTIME_DIR="$tmpdir/xdg-runtime" \
        "$CUBE" "$@"
}

rm -f "$PWD/controller-help.out" "$PWD/controller-help.err"
rm -f "$PWD/manager-help.out" "$PWD/manager-help.err"
rm -f "$PWD/cube-help.out" "$PWD/cube-help.err"
rm -f "$PWD/cube-ps-help.out" "$PWD/cube-ps-help.err"
rm -f "$PWD/cube-kill-help.out" "$PWD/cube-kill-help.err"
rm -f "$PWD/cube-connect-help.out" "$PWD/cube-connect-help.err"
rm -f "$PWD/cube-logs-help.out" "$PWD/cube-logs-help.err"
rm -f "$PWD/cube-run-help.out" "$PWD/cube-run-help.err"
rm -f "$PWD/cube-restart-help.out" "$PWD/cube-restart-help.err"
rm -f "$PWD/cube-save-help.out" "$PWD/cube-save-help.err"
rm -f "$PWD/cube-defaults-help.out" "$PWD/cube-defaults-help.err"
rm -f "$PWD/cube-missing-manager.out" "$PWD/cube-missing-manager.err"
rm -f "$PWD/cube-defaults.out" "$PWD/cube-defaults.err"
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
grep -q 'daemon \[--foreground\] \[--control-socket PATH\] \[--listen URI\] \[--allow-insecure\] \[--event-interval-ms N\] \[--max-clients N\]' "$PWD/manager-help.err"
grep -q 'Global options: --config PATH' "$PWD/manager-help.err"

cube --help >"$PWD/cube-help.out" 2>"$PWD/cube-help.err"
if [ -s "$PWD/cube-help.err" ]; then
    echo "cube help should write to stdout only" >&2
    exit 1
fi
grep -q 'Usage:' "$PWD/cube-help.out"
grep -q 'cube \[--config PATH\]' "$PWD/cube-help.out"
grep -q 'cube workspace \[NAME\]' "$PWD/cube-help.out"
grep -q 'cube run \[--fg|--bg\] \[--stream|--tty|--term\].*\[--restart\]' "$PWD/cube-help.out"
grep -q 'cube ps \[-a|--all\]' "$PWD/cube-help.out"
grep -q 'cube inspect NAME' "$PWD/cube-help.out"
grep -q 'cube logs \[--follow\] \[--stdout|--stderr\] \[--start N\] \[--end N\] NAME' "$PWD/cube-help.out"
grep -q 'cube events \[--follow \[--iterations N\]\]' "$PWD/cube-help.out"
grep -q 'cube signal NAME SIGNAL' "$PWD/cube-help.out"
grep -q 'cube restart NAME' "$PWD/cube-help.out"
grep -q 'cube kill \[--all\] \[--cleanup\] \[NAME\]' "$PWD/cube-help.out"
grep -q 'cube save NAME' "$PWD/cube-help.out"
grep -q 'cube unsave NAME' "$PWD/cube-help.out"
grep -q 'cube remove NAME' "$PWD/cube-help.out"
grep -q 'cube config show|effective|paths|validate' "$PWD/cube-help.out"
grep -q 'cube defaults show|set|reset' "$PWD/cube-help.out"
grep -q 'cube cleanup' "$PWD/cube-help.out"
grep -q 'cube access list|add|set-role|remove|revoke' "$PWD/cube-help.out"
grep -q 'cube connect \[--ro\] NAME' "$PWD/cube-help.out"

cube ps --help >"$PWD/cube-ps-help.out" 2>"$PWD/cube-ps-help.err"
if [ -s "$PWD/cube-ps-help.err" ]; then
    echo "cube ps help should write to stdout only" >&2
    exit 1
fi
grep -q 'cube ps \[-a|--all\]' "$PWD/cube-ps-help.out"

cube kill --help >"$PWD/cube-kill-help.out" 2>"$PWD/cube-kill-help.err"
if [ -s "$PWD/cube-kill-help.err" ]; then
    echo "cube kill help should write to stdout only" >&2
    exit 1
fi
grep -q 'cube kill \[--cleanup\] NAME' "$PWD/cube-kill-help.out"
grep -q 'cube kill --all \[--cleanup\]' "$PWD/cube-kill-help.out"

cube connect --help >"$PWD/cube-connect-help.out" 2>"$PWD/cube-connect-help.err"
if [ -s "$PWD/cube-connect-help.err" ]; then
    echo "cube connect help should write to stdout only" >&2
    exit 1
fi
grep -q 'cube connect \[--ro\] NAME' "$PWD/cube-connect-help.out"

cube logs --help >"$PWD/cube-logs-help.out" 2>"$PWD/cube-logs-help.err"
if [ -s "$PWD/cube-logs-help.err" ]; then
    echo "cube logs help should write to stdout only" >&2
    exit 1
fi
grep -q 'cube logs \[--follow\] \[--stdout|--stderr\] \[--start N\] \[--end N\] NAME' "$PWD/cube-logs-help.out"

cube restart --help >"$PWD/cube-restart-help.out" 2>"$PWD/cube-restart-help.err"
if [ -s "$PWD/cube-restart-help.err" ]; then
    echo "cube restart help should write to stdout only" >&2
    exit 1
fi
grep -q 'cube restart NAME' "$PWD/cube-restart-help.out"

cube run --help >"$PWD/cube-run-help.out" 2>"$PWD/cube-run-help.err"
if [ -s "$PWD/cube-run-help.err" ]; then
    echo "cube run help should write to stdout only" >&2
    exit 1
fi
grep -q 'cube run \[--fg|--bg\] \[--stream|--tty|--term\].*\[--restart\]' "$PWD/cube-run-help.out"

cube save --help >"$PWD/cube-save-help.out" 2>"$PWD/cube-save-help.err"
if [ -s "$PWD/cube-save-help.err" ]; then
    echo "cube save help should write to stdout only" >&2
    exit 1
fi
grep -q 'cube save NAME' "$PWD/cube-save-help.out"

cube defaults --help >"$PWD/cube-defaults-help.out" 2>"$PWD/cube-defaults-help.err"
if [ -s "$PWD/cube-defaults-help.err" ]; then
    echo "cube defaults help should write to stdout only" >&2
    exit 1
fi
grep -q 'cube defaults \[show\]' "$PWD/cube-defaults-help.out"
grep -q 'cube defaults set launch foreground|background' "$PWD/cube-defaults-help.out"
grep -q 'cube defaults reset \[launch|mode|kill-cleanup\]' "$PWD/cube-defaults-help.out"

set +e
cube --manager-socket "$PWD/missing-manager.sock" --workspace "Project A" ps \
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

cube --manager-socket "$PWD/missing-manager.sock" defaults \
    >"$PWD/cube-defaults.out" 2>"$PWD/cube-defaults.err"
grep -q '^launch=' "$PWD/cube-defaults.out"
grep -q '^mode=' "$PWD/cube-defaults.out"
grep -q '^kill_cleanup=' "$PWD/cube-defaults.out"
if [ -s "$PWD/cube-defaults.err" ]; then
    echo "cube defaults should not require manager connection" >&2
    exit 1
fi

set +e
cube wat >"$PWD/cube-unknown.out" 2>"$PWD/cube-unknown.err"
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
rm -f "$PWD/cube-kill-help.out" "$PWD/cube-kill-help.err"
rm -f "$PWD/cube-connect-help.out" "$PWD/cube-connect-help.err"
rm -f "$PWD/cube-logs-help.out" "$PWD/cube-logs-help.err"
rm -f "$PWD/cube-save-help.out" "$PWD/cube-save-help.err"
rm -f "$PWD/cube-defaults-help.out" "$PWD/cube-defaults-help.err"
rm -f "$PWD/cube-missing-manager.out" "$PWD/cube-missing-manager.err"
rm -f "$PWD/cube-defaults.out" "$PWD/cube-defaults.err"
rm -f "$PWD/cube-unknown.out" "$PWD/cube-unknown.err"
