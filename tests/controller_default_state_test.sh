set -eu

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

cd "$tmpdir"

"$CUBICLE_CONTROLLER" --mode stream -- sh -c 'printf "default-state\n"'

state_count=$(find .cubicle/controllers -mindepth 1 -maxdepth 1 -type d | wc -l)
if [ "$state_count" -ne 1 ]; then
    echo "expected one default controller state directory, found $state_count" >&2
    exit 1
fi

state_dir=$(find .cubicle/controllers -mindepth 1 -maxdepth 1 -type d)
state_name=${state_dir##*/}

grep -q '^controller_id=[0-9a-f][0-9a-f][0-9a-f][0-9a-f]' "$state_dir/metadata"
controller_id=$(sed -n 's/^controller_id=//p' "$state_dir/metadata")
child_pid=$(sed -n 's/^pid=//p' "$state_dir/metadata")

if [ "$state_name" != "$controller_id" ]; then
    echo "state directory name does not match controller_id metadata" >&2
    exit 1
fi

if [ "$state_name" = "$child_pid" ]; then
    echo "default state directory should not be pid-derived: $state_name" >&2
    exit 1
fi

printf "default-state\n" | cmp - "$state_dir/stdout.log"
grep -q "type=process_started controller_id=$controller_id " "$state_dir/events.log"
