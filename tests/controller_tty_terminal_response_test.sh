set -eu

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

state_dir="$tmpdir/state"

"$CUBICLE_CONTROLLER" --state-dir "$state_dir" --mode tty --stdin-policy open -- \
    python3 -c '
import os
import select
import sys
import termios
import tty
import time

tty.setraw(0)
sys.stdout.write("\x1b[6n\x1b]10;?\x1b\\\x1b]11;?\x1b\\\x1b[?u\x1b[c")
sys.stdout.flush()

data = b""
deadline = time.time() + 3
while time.time() < deadline:
    ready, _, _ = select.select([sys.stdin], [], [], 0.05)
    if ready:
        data += os.read(0, 1024)
    if (b"\x1b[1;1R" in data and
            b"\x1b]10;rgb:" in data and
            b"\x1b]11;rgb:" in data and
            b"\x1b[?0u" in data and
            b"\x1b[?1;2c" in data):
        break

sys.stdout.write("\nGOT_RESPONSES\n" if data else "\nNO_RESPONSE\n")
sys.stdout.flush()
if not data:
    sys.exit(1)
' >/dev/null 2>"$tmpdir/stderr"

grep -q 'GOT_RESPONSES' "$state_dir/stdout.log"
grep -q 'type=terminal_response query=dsr ' "$state_dir/events.log"
grep -q 'type=terminal_response query=foreground-color ' "$state_dir/events.log"
grep -q 'type=terminal_response query=background-color ' "$state_dir/events.log"
grep -q 'type=terminal_response query=keyboard-protocol ' "$state_dir/events.log"
grep -q 'type=terminal_response query=primary-da ' "$state_dir/events.log"
