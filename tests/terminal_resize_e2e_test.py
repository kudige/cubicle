#!/usr/bin/env python3
import json
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
import fcntl


def set_window_size(fd, rows, columns):
    fcntl.ioctl(fd, termios.TIOCSWINSZ,
                struct.pack("HHHH", rows, columns, 0, 0))


def read_available(fd, duration=0.05):
    output = bytearray()
    deadline = time.monotonic() + duration
    while time.monotonic() < deadline:
        ready, _, _ = select.select([fd], [], [], 0.01)
        if not ready:
            continue
        try:
            chunk = os.read(fd, 4096)
        except OSError:
            break
        if not chunk:
            break
        output.extend(chunk)
    return output.decode(errors="replace").replace("\r", "")


def read_until(fd, needle, timeout=5):
    output = ""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        output += read_available(fd, 0.05)
        if needle in output:
            return output
    return output


def load_events(path):
    if not os.path.exists(path):
        return []
    events = []
    with open(path, "r", encoding="utf-8") as event_file:
        for line in event_file:
            events.append(json.loads(line))
    return events


def wait_for_event(path, predicate, description, after=0):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        events = load_events(path)
        for index, event in enumerate(events[after:], start=after):
            if predicate(event):
                return index + 1
        time.sleep(0.05)
    raise AssertionError(f"missing recorder event: {description}; got {load_events(path)!r}")


def event_is(name, rows=None, columns=None):
    def predicate(event):
        if event.get("event") != name:
            return False
        if rows is not None and event.get("rows") != rows:
            return False
        if columns is not None and event.get("columns") != columns:
            return False
        return True
    return predicate


def wait_for_socket(socket_path):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if os.path.exists(socket_path):
            return
        time.sleep(0.05)
    raise AssertionError("control socket was not created")


def run_controller(controller, recorder, state_dir, socket_path, output_path,
                   stderr_path, stdin, stdout):
    with open(stderr_path, "wb") as stderr_file:
        return subprocess.Popen(
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
                sys.executable,
                recorder,
                "--output",
                output_path,
            ],
            stdin=stdin,
            stdout=stdout,
            stderr=stderr_file,
            close_fds=True,
        )


def start_socket_attach(control_client, socket_path, rows, columns):
    master_fd, slave_fd = pty.openpty()
    set_window_size(master_fd, rows, columns)
    process = subprocess.Popen(
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
    return master_fd, process


def wait_process(process, stderr_path=None):
    status = process.wait(timeout=5)
    if status != 0:
        stderr = ""
        if stderr_path is not None and os.path.exists(stderr_path):
            with open(stderr_path, "rb") as stderr_file:
                stderr = stderr_file.read().decode(errors="replace")
        elif process.stderr is not None:
            stderr = process.stderr.read().decode(errors="replace")
        raise AssertionError(f"process exited {status}: {stderr}")


def cleanup_process(process):
    if process.poll() is None:
        process.terminate()
        process.wait(timeout=5)


def controller_and_socket_resize(controller, control_client, recorder):
    with tempfile.TemporaryDirectory() as tmpdir:
        state_dir = os.path.join(tmpdir, "state")
        socket_path = os.path.join(tmpdir, "control.sock")
        output_path = os.path.join(tmpdir, "recorder.jsonl")
        stderr_path = os.path.join(tmpdir, "controller.stderr")

        controller_master, controller_slave = pty.openpty()
        set_window_size(controller_master, 30, 90)
        controller_process = run_controller(
            controller, recorder, state_dir, socket_path, output_path,
            stderr_path, controller_slave, controller_slave
        )
        os.close(controller_slave)
        socket_master = -1
        socket_process = None

        try:
            wait_for_socket(socket_path)
            cursor = wait_for_event(output_path, event_is("start", 30, 90),
                                    "foreground start 30x90")

            set_window_size(controller_master, 34, 100)
            os.kill(controller_process.pid, signal.SIGWINCH)
            cursor = wait_for_event(output_path, event_is("resize", 34, 100),
                                    "foreground resize 34x100", cursor)

            socket_master, socket_process = start_socket_attach(
                control_client, socket_path, 20, 60
            )
            cursor = wait_for_event(output_path, event_is("resize", 20, 60),
                                    "socket resize 20x60", cursor)
            border = read_until(socket_master, "size=20x60")
            if "size=20x60" not in border:
                raise AssertionError(f"socket did not render 20x60 border: {border!r}")

            os.write(socket_master, b"\x03")
            cursor = wait_for_event(output_path, event_is("control_c"),
                                    "socket control-c", cursor)

            os.write(socket_master, b"q")
            wait_for_event(output_path, event_is("exit"), "socket exit key", cursor)
            wait_process(controller_process, stderr_path)
            wait_process(socket_process)
        finally:
            if socket_master >= 0:
                os.close(socket_master)
            os.close(controller_master)
            cleanup_process(controller_process)
            if socket_process is not None:
                cleanup_process(socket_process)


def socket_to_socket_resize(controller, control_client, recorder):
    with tempfile.TemporaryDirectory() as tmpdir:
        state_dir = os.path.join(tmpdir, "state")
        socket_path = os.path.join(tmpdir, "control.sock")
        output_path = os.path.join(tmpdir, "recorder.jsonl")
        stderr_path = os.path.join(tmpdir, "controller.stderr")

        with open(os.devnull, "rb") as devnull_in, open(os.devnull, "wb") as devnull_out:
            controller_process = run_controller(
                controller, recorder, state_dir, socket_path, output_path,
                stderr_path, devnull_in, devnull_out
            )

        first_master = -1
        second_master = -1
        first_process = None
        second_process = None

        try:
            wait_for_socket(socket_path)
            cursor = wait_for_event(output_path, lambda event: event.get("event") == "start",
                                    "daemon start")

            first_master, first_process = start_socket_attach(
                control_client, socket_path, 42, 120
            )
            cursor = wait_for_event(output_path, event_is("resize", 42, 120),
                                    "first socket resize 42x120", cursor)
            first_border = read_until(first_master, "size=42x120")
            if "size=42x120" not in first_border:
                raise AssertionError(f"first socket did not render 42x120 border: {first_border!r}")

            second_master, second_process = start_socket_attach(
                control_client, socket_path, 22, 70
            )
            cursor = wait_for_event(output_path, event_is("resize", 22, 70),
                                    "second socket resize 22x70", cursor)
            second_border = read_until(second_master, "size=22x70")
            if "size=22x70" not in second_border:
                raise AssertionError(f"second socket did not render 22x70 border: {second_border!r}")

            time.sleep(1.1)
            os.write(first_master, b"a")
            cursor = wait_for_event(output_path, event_is("input"),
                                    "first socket input", cursor)
            cursor = wait_for_event(output_path, event_is("resize", 42, 120),
                                    "first socket active resize reassert", cursor)

            os.write(second_master, b"q")
            wait_for_event(output_path, event_is("exit"),
                           "second socket exit key", cursor)
            wait_process(controller_process, stderr_path)
            wait_process(first_process)
            wait_process(second_process)
        finally:
            if first_master >= 0:
                os.close(first_master)
            if second_master >= 0:
                os.close(second_master)
            cleanup_process(controller_process)
            if first_process is not None:
                cleanup_process(first_process)
            if second_process is not None:
                cleanup_process(second_process)


def main():
    controller = os.environ["CUBICLE_CONTROLLER"]
    control_client = os.environ["CUBICLE_CONTROL_CLIENT"]
    recorder = os.environ["CUBICLE_TERMINAL_RECORDER"]

    controller_and_socket_resize(controller, control_client, recorder)
    socket_to_socket_resize(controller, control_client, recorder)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(exc, file=sys.stderr)
        sys.exit(1)
