set -eu

tmpdir=$(mktemp -d)
cleanup() {
    rm -rf "$tmpdir"
}
trap cleanup EXIT

src_dir=${CUBICLE_SOURCE_DIR:?}

${CC:-cc} -std=c17 -Wall -Wextra -Wpedantic \
    -I "$src_dir/include" \
    "$src_dir/tests/libcubicle_real_manager_test.c" \
    "$src_dir/src/client/client.c" \
    "$src_dir/src/client/error.c" \
    "$src_dir/src/client/transport_unix.c" \
    "$src_dir/src/common/process.c" \
    "$src_dir/src/common/rpc.c" \
    -o "$tmpdir/libcubicle-real-manager-test"

"$tmpdir/libcubicle-real-manager-test"
