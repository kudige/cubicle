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


def read_until(fd, needle, deadline):
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
        if needle in text:
            return text
    return output.decode(errors="replace").replace("\r", "")


def main():
    controller = os.environ["CUBICLE_CONTROLLER"]

    with tempfile.TemporaryDirectory() as tmpdir:
        state_dir = os.path.join(tmpdir, "state")
        master_fd, slave_fd = pty.openpty()
        set_window_size(master_fd, 33, 101)

        command = """
import os
import signal
import sys


def terminal_size():
    size = os.get_terminal_size(sys.stdout.fileno())
    return f"{size.lines} {size.columns}"


def handle_winch(_signal_number, _frame):
    print(f"winch:{terminal_size()}", flush=True)


signal.signal(signal.SIGWINCH, handle_winch)
print(f"first:{terminal_size()}", flush=True)
line = sys.stdin.buffer.readline().decode().rstrip("\\n")
print(f"typed:{line}", flush=True)
"""
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
                sys.executable,
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
                output += read_until(master_fd, "first:33 101",
                                     time.monotonic() + 0.1)
                if "first:33 101" in output:
                    break
            if "first:33 101" not in output:
                raise AssertionError(f"missing initial terminal size in: {output!r}")

            set_window_size(master_fd, 40, 120)
            os.kill(process.pid, signal.SIGWINCH)
            output += read_until(master_fd, "winch:40 120", time.monotonic() + 5)
            if "winch:40 120" not in output:
                raise AssertionError(f"missing resize notification before input in: {output!r}")

            os.write(master_fd, b"hello from local tty\n")

            output += read_until(master_fd, "typed:hello from local tty",
                                 time.monotonic() + 5)
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
