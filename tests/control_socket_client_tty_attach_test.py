#!/usr/bin/env python3
import fcntl
import os
import pty
import select
import struct
import subprocess
import sys
import tempfile
import time
import termios


def set_window_size(fd, rows, columns):
    fcntl.ioctl(fd, termios.TIOCSWINSZ,
                struct.pack("HHHH", rows, columns, 0, 0))


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


def wait_for_socket_and_output(socket_path, stdout_log):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if os.path.exists(socket_path) and os.path.exists(stdout_log):
            with open(stdout_log, "rb") as log_file:
                if b"ready" in log_file.read():
                    return
        time.sleep(0.05)
    raise AssertionError("controller did not become ready")


def main():
    controller = os.environ["CUBICLE_CONTROLLER"]
    control_client = os.environ["CUBICLE_CONTROL_CLIENT"]

    with tempfile.TemporaryDirectory() as tmpdir:
        state_dir = os.path.join(tmpdir, "state")
        socket_path = os.path.join(tmpdir, "control.sock")
        stdout_log = os.path.join(state_dir, "stdout.log")
        controller_stderr = os.path.join(tmpdir, "controller-stderr")

        command = (
            'printf "ready"; '
            'IFS= read -r line; '
            'printf "size:%s\\n" "$(stty size)"; '
            'printf "typed:%s\\n" "$line"'
        )
        with open(os.devnull, "wb") as devnull, open(controller_stderr, "wb") as stderr_file:
            controller_process = subprocess.Popen(
                [
                    controller,
                    "--state-dir",
                    state_dir,
                    "--control-socket",
                    socket_path,
                    "--mode",
                    "tty",
                    "--stdin-policy",
                    "open",
                    "--",
                    "sh",
                    "-c",
                    command,
                ],
                stdin=subprocess.DEVNULL,
                stdout=devnull,
                stderr=stderr_file,
                close_fds=True,
            )

        master_fd, slave_fd = pty.openpty()
        set_window_size(master_fd, 44, 132)

        try:
            wait_for_socket_and_output(socket_path, stdout_log)

            client_process = subprocess.Popen(
                [
                    sys.executable,
                    control_client,
                    socket_path,
                    "attach",
                    "tty",
                ],
                stdin=slave_fd,
                stdout=slave_fd,
                stderr=subprocess.PIPE,
                close_fds=True,
            )
            os.close(slave_fd)
            slave_fd = -1

            output = read_until(master_fd, "ready", time.monotonic() + 5)
            if "ready" not in output:
                raise AssertionError(f"missing attach catch-up output: {output!r}")

            os.write(master_fd, b"hello from attach tty\n")
            output += read_until(master_fd, "typed:hello from attach tty",
                                 time.monotonic() + 5)
            if "size:44 132" not in output:
                raise AssertionError(f"missing socket resize result: {output!r}")
            if "typed:hello from attach tty" not in output:
                raise AssertionError(f"missing socket input result: {output!r}")

            controller_status = controller_process.wait(timeout=5)
            if controller_status != 0:
                with open(controller_stderr, "rb") as stderr_file:
                    stderr = stderr_file.read().decode(errors="replace")
                raise AssertionError(f"controller exited {controller_status}: {stderr}")

            client_status = client_process.wait(timeout=5)
            if client_status != 0:
                stderr = client_process.stderr.read().decode(errors="replace")
                raise AssertionError(f"client exited {client_status}: {stderr}")

            with open(os.path.join(state_dir, "events.log"), "rb") as events_file:
                events = events_file.read().decode(errors="replace")
            if "type=terminal_resized rows=44 columns=132" not in events:
                raise AssertionError(f"missing resize event: {events!r}")
            if "type=client_attached stream=stdout" not in events:
                raise AssertionError(f"missing stdout attach event: {events!r}")
            if "type=client_attached stream=stdin" not in events:
                raise AssertionError(f"missing stdin attach event: {events!r}")
        finally:
            if slave_fd >= 0:
                os.close(slave_fd)
            os.close(master_fd)
            if controller_process.poll() is None:
                controller_process.terminate()
                controller_process.wait(timeout=5)
            if "client_process" in locals() and client_process.poll() is None:
                client_process.terminate()
                client_process.wait(timeout=5)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(exc, file=sys.stderr)
        sys.exit(1)
