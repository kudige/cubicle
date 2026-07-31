#!/usr/bin/env python3
import fcntl
import os
import pty
import select
import signal
import struct
import subprocess
import sys
import tempfile
import time
import termios


def set_window_size(fd, rows, cols):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


def read_available(fd, deadline):
    output = bytearray()
    while time.monotonic() < deadline:
        ready, _, _ = select.select([fd], [], [], 0.05)
        if not ready:
            continue
        try:
            chunk = os.read(fd, 4096)
        except OSError:
            break
        if not chunk:
            break
        output.extend(chunk)
        text = output.decode(errors="replace").replace("\r", "")
        if "typed:hello from local tty" in text:
            return text
    return output.decode(errors="replace").replace("\r", "")


def main():
    controller = os.environ["CUBICLE_CONTROLLER"]

    with tempfile.TemporaryDirectory() as tmpdir:
        state_dir = os.path.join(tmpdir, "state")
        master_fd, slave_fd = pty.openpty()
        set_window_size(master_fd, 33, 101)

        command = (
            'printf "first:%s\\n" "$(stty size)"; '
            'sleep 0.4; '
            'printf "second:%s\\n" "$(stty size)"; '
            'IFS= read -r line; '
            'printf "typed:%s\\n" "$line"'
        )
        process = subprocess.Popen(
            [
                controller,
                "--state-dir",
                state_dir,
                "--mode",
                "tty",
                "--stdin-policy",
                "open",
                "--",
                "sh",
                "-c",
                command,
            ],
            stdin=slave_fd,
            stdout=slave_fd,
            stderr=subprocess.PIPE,
            close_fds=True,
        )
        os.close(slave_fd)

        try:
            deadline = time.monotonic() + 5
            output = ""
            while time.monotonic() < deadline:
                output += read_available(master_fd, time.monotonic() + 0.1)
                if "first:33 101" in output:
                    break
            if "first:33 101" not in output:
                raise AssertionError(f"missing initial terminal size in: {output!r}")

            set_window_size(master_fd, 40, 120)
            os.kill(process.pid, signal.SIGWINCH)
            time.sleep(0.1)
            os.write(master_fd, b"hello from local tty\n")

            output += read_available(master_fd, time.monotonic() + 5)
            if "second:40 120" not in output:
                raise AssertionError(f"missing resized terminal size in: {output!r}")
            if "typed:hello from local tty" not in output:
                raise AssertionError(f"missing forwarded local input in: {output!r}")

            status = process.wait(timeout=5)
            if status != 0:
                stderr = process.stderr.read().decode(errors="replace")
                raise AssertionError(f"controller exited {status}: {stderr}")

            with open(os.path.join(state_dir, "stdout.log"), "rb") as log_file:
                log = log_file.read().decode(errors="replace").replace("\r", "")
            if "typed:hello from local tty" not in log:
                raise AssertionError(f"missing forwarded input in stdout log: {log!r}")
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=5)
            os.close(master_fd)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(exc, file=sys.stderr)
        sys.exit(1)
