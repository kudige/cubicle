set -eu

tmpdir=$(mktemp -d)
cleanup() {
    rm -rf "$tmpdir"
}
trap cleanup EXIT

src_dir=${CUBICLE_SOURCE_DIR:?}

${CC:-cc} -std=c17 -Wall -Wextra -Wpedantic \
    -I "$src_dir/include" \
    -I "$src_dir/third_party/yyjson" \
    "$src_dir/tests/libcubicle_real_manager_test.c" \
    "$src_dir"/src/client/*.c \
    "$src_dir/src/common/json.c" \
    "$src_dir/src/common/process.c" \
    "$src_dir/src/common/rpc.c" \
    "$src_dir/third_party/yyjson/yyjson.c" \
    -o "$tmpdir/libcubicle-real-manager-test"

"$tmpdir/libcubicle-real-manager-test"
