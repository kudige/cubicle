#!/usr/bin/env python3
import os
import pty
import re
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time


CSI_RE = re.compile(rb"\x1b\[[0-?]*[ -/]*[@-~]")
OSC_RE = re.compile(rb"\x1b\][^\x07]*(?:\x07|\x1b\\)")
SIMPLE_ESC_RE = re.compile(rb"\x1b[][()#%*+\-. /0-9:;<=>?A-Za-z]")

NORMAL_CURSOR_KEYS = {
    "up": b"\x1b[A",
    "down": b"\x1b[B",
    "right": b"\x1b[C",
    "left": b"\x1b[D",
}

APPLICATION_CURSOR_KEYS = {
    "up": b"\x1bOA",
    "down": b"\x1bOB",
    "right": b"\x1bOC",
    "left": b"\x1bOD",
}


def strip_terminal_sequences(data):
    data = OSC_RE.sub(b"", data)
    data = CSI_RE.sub(b"", data)
    data = SIMPLE_ESC_RE.sub(b"", data)
    return data


def set_window_size(fd, rows, columns):
    termios_size = struct.pack("HHHH", rows, columns, 0, 0)
    import fcntl

    fcntl.ioctl(fd, termios.TIOCSWINSZ, termios_size)


def wait_for_path(path, description, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if os.path.exists(path):
            return
        time.sleep(0.05)
    raise AssertionError(f"timed out waiting for {description}: {path}")


class PtyProcess:
    def __init__(self, command, env, rows=24, cols=80):
        master_fd, slave_fd = pty.openpty()
        set_window_size(slave_fd, rows, cols)
        self.master_fd = master_fd
        self.output = bytearray()
        self.process = subprocess.Popen(
            command,
            stdin=slave_fd,
            stdout=slave_fd,
            stderr=slave_fd,
            env=env,
            close_fds=True,
        )
        os.close(slave_fd)

    def write(self, data):
        os.write(self.master_fd, data)

    def send_user_arrow(self, direction, surface):
        """Send the bytes a user's terminal would send to this client surface.

        For direct `cube connect`, the attached program can put the user's real
        terminal into application cursor mode, so arrow keys arrive as ESC O*.
        For `desk`, the inner program's modes are rendered inside a pane and
        must not mutate the outer terminal, so normal cursor keys arrive as
        ESC [* and desk is responsible for translating them for the pane.
        """
        normalized = direction.lower()
        if surface == "cube-connect":
            keys = APPLICATION_CURSOR_KEYS
        elif surface == "desk":
            keys = NORMAL_CURSOR_KEYS
        else:
            raise AssertionError(f"unknown client surface: {surface!r}")
        if normalized not in keys:
            raise AssertionError(f"unknown arrow direction: {direction!r}")
        self.write(keys[normalized])

    def read_available(self, quiet=0.05):
        while True:
            readable, _, _ = select.select([self.master_fd], [], [], quiet)
            if not readable:
                break
            try:
                chunk = os.read(self.master_fd, 8192)
            except OSError:
                break
            if not chunk:
                break
            self.output.extend(chunk)
            quiet = 0.01
        return bytes(self.output)

    def wait_for_text(self, text, timeout=5):
        needle = text.encode("utf-8")
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.read_available()
            if needle in strip_terminal_sequences(bytes(self.output)):
                return bytes(self.output)
            if self.process.poll() is not None:
                break
        clean = strip_terminal_sequences(bytes(self.output)).decode(
            "utf-8", errors="replace"
        )
        raise AssertionError(f"did not read {text!r}; captured:\n{clean}")

    def terminate(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=2)
        os.close(self.master_fd)

    def wait(self, timeout=5):
        return self.process.wait(timeout=timeout)


class CubicleEndUserHarness:
    def __init__(self):
        self.cube = os.environ["CUBE"]
        self.desk = os.environ.get("DESK")
        self.manager = os.environ["CUBICLE_MANAGER"]
        self.controller = os.environ["CUBICLE_CONTROLLER"]
        self.api_client = os.environ.get("CUBICLE_API_CLIENT")
        self.tempdir = tempfile.TemporaryDirectory(prefix="cubicle-end-user.")
        self.root = self.tempdir.name
        self.state_dir = os.path.join(self.root, "manager-state")
        self.log_dir = os.path.join(self.root, "log")
        self.socket_path = os.path.join(self.root, "manager.sock")
        self.manager_process = None
        self.env = os.environ.copy()
        self.env["XDG_STATE_HOME"] = os.path.join(self.root, "xdg-state")
        self.env["XDG_RUNTIME_DIR"] = os.path.join(self.root, "runtime")
        self.env["XDG_CONFIG_HOME"] = os.path.join(self.root, "config")
        self.env["CUBICLE_MANAGER_SOCKET"] = self.socket_path
        self.env["TERM"] = "xterm-256color"
        self.env["LESS"] = ""
        self.env["LESSHISTFILE"] = os.path.join(self.root, "less-history")
        for path in (
            self.env["XDG_STATE_HOME"],
            self.env["XDG_RUNTIME_DIR"],
            os.path.join(self.env["XDG_CONFIG_HOME"], "cubicle"),
        ):
            os.makedirs(path, exist_ok=True)
        self._write_config()

    def _write_config(self):
        config_path = os.path.join(
            self.env["XDG_CONFIG_HOME"], "cubicle", "config.cfg"
        )
        with open(config_path, "w", encoding="utf-8") as handle:
            handle.write(
                "[manager]\n"
                f"log_dir={self.log_dir}\n"
                "\n"
                "[controller]\n"
                "debug=input\n"
                "\n"
                "[desk]\n"
                "debug=library,terminal\n"
            )

    def start_manager(self):
        self.manager_process = subprocess.Popen(
            [
                self.manager,
                "--state-dir",
                self.state_dir,
                "--log-dir",
                self.log_dir,
                "--controller-bin",
                self.controller,
                "daemon",
                "--foreground",
                "--control-socket",
                self.socket_path,
                "--event-interval-ms",
                "50",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=self.env,
        )
        wait_for_path(self.socket_path, "manager socket")
        if self.api_client:
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                ping = subprocess.run(
                    [sys.executable, self.api_client, self.socket_path, "ping"],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    env=self.env,
                )
                if ping.returncode == 0:
                    return
                time.sleep(0.05)
            raise AssertionError("manager did not become ready")

    def run_cube(self, args, **kwargs):
        command = [self.cube] + list(args)
        result = subprocess.run(
            command,
            env=self.env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            **kwargs,
        )
        if result.returncode != 0:
            raise AssertionError(
                f"{command!r} failed with {result.returncode}\n"
                f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
            )
        return result.stdout

    def wait_for_cube_output(self, args, text, timeout=5):
        deadline = time.monotonic() + timeout
        last = ""
        while time.monotonic() < deadline:
            result = subprocess.run(
                [self.cube] + list(args),
                env=self.env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            last = result.stdout + result.stderr
            if text in last:
                return last
            time.sleep(0.05)
        raise AssertionError(f"missing {text!r}; last output:\n{last}")

    def spawn_cube_pty(self, args, rows=24, cols=80):
        return PtyProcess([self.cube] + list(args), self.env, rows, cols)

    def spawn_desk_pty(self, args=None, rows=24, cols=80):
        if self.desk is None:
            raise AssertionError("DESK was not provided")
        return PtyProcess([self.desk] + list(args or []), self.env, rows, cols)

    def make_long_file(self, name="long.txt", lines=200):
        path = os.path.join(self.root, name)
        with open(path, "w", encoding="utf-8") as handle:
            for index in range(1, lines + 1):
                handle.write(f"LESS_LINE_{index:04d}\n")
        return path

    def cleanup(self):
        try:
            subprocess.run(
                [self.cube, "kill", "--all", "--cleanup"],
                env=self.env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=5,
            )
        except (subprocess.TimeoutExpired, FileNotFoundError):
            pass
        if self.manager_process is not None:
            if self.manager_process.poll() is None:
                self.manager_process.send_signal(signal.SIGTERM)
                try:
                    self.manager_process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.manager_process.kill()
                    self.manager_process.wait(timeout=5)
            self.manager_process = None
        self.tempdir.cleanup()

    def __enter__(self):
        self.start_manager()
        return self

    def __exit__(self, _exc_type, _exc, _tb):
        self.cleanup()
