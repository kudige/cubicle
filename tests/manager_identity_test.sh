set -eu

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

file_mode() {
    if stat -c '%a' "$1" >/dev/null 2>&1; then
        stat -c '%a' "$1"
    else
        stat -f '%Lp' "$1"
    fi
}

state_dir="$tmpdir/manager"

"$CUBICLE_MANAGER" --state-dir "$state_dir" workspace list \
    >"$tmpdir/list.out" 2>"$tmpdir/list.err"

test -f "$state_dir/keys/manager.key"
test -f "$state_dir/keys/manager.pub"
test "$(file_mode "$state_dir/keys")" = "700"
test "$(file_mode "$state_dir/keys/manager.key")" = "600"

first_public=$(cat "$state_dir/keys/manager.pub")
"$CUBICLE_MANAGER" --state-dir "$state_dir" workspace list \
    >"$tmpdir/list2.out" 2>"$tmpdir/list2.err"
second_public=$(cat "$state_dir/keys/manager.pub")
test "$first_public" = "$second_public"

chmod 0644 "$state_dir/keys/manager.key"
if "$CUBICLE_MANAGER" --state-dir "$state_dir" workspace list \
    >"$tmpdir/unsafe.out" 2>"$tmpdir/unsafe.err"; then
    echo "manager accepted unsafe private key permissions" >&2
    exit 1
fi
grep -q 'failed to initialize manager identity' "$tmpdir/unsafe.err"
