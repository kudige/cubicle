set -eu

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
grep -q 'events follow --iterations N' "$PWD/manager-help.err"

rm -f "$PWD/controller-help.out" "$PWD/controller-help.err"
rm -f "$PWD/manager-help.out" "$PWD/manager-help.err"
