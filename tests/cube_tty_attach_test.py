#!/usr/bin/env python3
import json
import os
import pty
import select
import struct
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import termios
import fcntl


def wait_for_path(path, description):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if os.path.exists(path):
            return
        time.sleep(0.05)
    raise AssertionError(f"timed out waiting for {description}: {path}")


def load_events(path):
    if not os.path.exists(path):
        return []
    with open(path, "r", encoding="utf-8") as event_file:
        return [json.loads(line) for line in event_file]


def wait_for_event(path, predicate, description):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        events = load_events(path)
        if any(predicate(event) for event in events):
            return events
        time.sleep(0.05)
    raise AssertionError(f"missing {description}: {load_events(path)!r}")


def read_available(fd):
    output = bytearray()
    while True:
        ready, _, _ = select.select([fd], [], [], 0.01)
        if not ready:
            break
        try:
            chunk = os.read(fd, 4096)
        except OSError:
            break
        if not chunk:
            break
        output.extend(chunk)
    return output


def read_until(fd, needle, timeout=5):
    output = bytearray()
    deadline = time.monotonic() + timeout
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
        if needle in output:
            return bytes(output)
    raise AssertionError(f"did not read {needle!r}; captured {bytes(output)!r}")


def set_window_size(fd, rows, columns):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))


def check_call(command, **kwargs):
    subprocess.check_call(command, **kwargs)


def check_output(command, **kwargs):
    return subprocess.check_output(command, text=True, **kwargs)


def wait_for_output(command, needle, env):
    deadline = time.monotonic() + 5
    last = ""
    while time.monotonic() < deadline:
        result = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=env,
        )
        last = result.stdout + result.stderr
        if needle in last:
            return last
        time.sleep(0.05)
    raise AssertionError(f"missing {needle!r}; last output {last!r}")


def main():
    cube = os.environ["CUBE"]
    manager = os.environ["CUBICLE_MANAGER"]
    controller = os.environ["CUBICLE_CONTROLLER"]
    api_client = os.environ["CUBICLE_API_CLIENT"]
    recorder = os.environ["CUBICLE_TERMINAL_RECORDER"]

    with tempfile.TemporaryDirectory() as tmpdir:
        state_dir = os.path.join(tmpdir, "manager")
        socket_path = os.path.join(tmpdir, "manager.sock")
        xdg_state_home = os.path.join(tmpdir, "xdg-state")
        recorder_output = os.path.join(tmpdir, "recorder.jsonl")

        workspace_output = check_output(
            [manager, "--state-dir", state_dir, "workspace", "create", "Project A"]
        )
        workspace_id = workspace_output.split("workspace id=", 1)[1].split(" name=", 1)[0]

        manager_process = subprocess.Popen(
            [
                manager,
                "--state-dir",
                state_dir,
                "--controller-bin",
                controller,
                "daemon",
                "--foreground",
                "--control-socket",
                socket_path,
                "--event-interval-ms",
                "50",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )

        master_fd = -1
        cube_process = None
        try:
            wait_for_path(socket_path, "manager socket")
            master_fd, slave_fd = pty.openpty()
            set_window_size(master_fd, 24, 80)
            env = os.environ.copy()
            env["CUBICLE_MANAGER_SOCKET"] = socket_path
            env["XDG_STATE_HOME"] = xdg_state_home
            cube_process = subprocess.Popen(
                [
                    cube,
                    "--workspace",
                    "Project A",
                    "run",
                    "--tty",
                    "--name",
                    "fg-tty",
                    sys.executable,
                    recorder,
                    "--output",
                    recorder_output,
                ],
                stdin=slave_fd,
                stdout=slave_fd,
                stderr=slave_fd,
                env=env,
                close_fds=True,
            )
            os.close(slave_fd)

            wait_for_event(
                recorder_output,
                lambda event: event.get("event") == "start",
                "foreground tty recorder start",
            )
            read_available(master_fd)
            os.write(master_fd, b"q")
            events = wait_for_event(
                recorder_output,
                lambda event: event.get("event") == "exit"
                and event.get("reason") == "exit_key",
                "foreground tty recorder exit",
            )
            if not any(
                event.get("event") == "input" and event.get("text") == "q"
                for event in events
            ):
                raise AssertionError(f"foreground tty input was not delivered: {events!r}")

            status = cube_process.wait(timeout=5)
            if status != 0:
                remaining = read_available(master_fd).decode(errors="replace")
                raise AssertionError(f"cube foreground tty exited {status}: {remaining}")

            ps_output = check_output(
                [
                    cube,
                    "--manager-socket",
                    socket_path,
                    "--workspace",
                    "Project A",
                    "ps",
                ],
                env=env,
            )
            if "fg-tty\ttty\tcompleted" not in ps_output:
                raise AssertionError(f"foreground tty process did not complete: {ps_output!r}")
            check_call(
                [
                    cube,
                    "--manager-socket",
                    socket_path,
                    "--workspace",
                    "Project A",
                    "remove",
                    "fg-tty",
                ],
                stdout=subprocess.DEVNULL,
                env=env,
            )

            check_call(
                [
                    sys.executable,
                    api_client,
                    socket_path,
                    "workspace-get",
                    workspace_id,
                ],
                stdout=subprocess.DEVNULL,
            )

            replay_probe = (
                "import os,select,sys,termios,tty,time\n"
                "tty.setraw(0)\n"
                "sys.stdout.write('\\x1b[6n')\n"
                "sys.stdout.flush()\n"
                "deadline = time.time() + 2.0\n"
                "data = b''\n"
                "while time.time() < deadline:\n"
                "    ready, _, _ = select.select([sys.stdin], [], [], 0.05)\n"
                "    if ready:\n"
                "        data = os.read(0, 64)\n"
                "        break\n"
                "sys.stdout.write('GOT_INPUT\\n' if data else 'NO_INPUT\\n')\n"
                "sys.stdout.flush()\n"
                "time.sleep(5)\n"
            )
            check_call(
                [
                    cube,
                    "--manager-socket",
                    socket_path,
                    "--workspace",
                    "Project A",
                    "run",
                    "--bg",
                    "--tty",
                    "--name",
                    "replay-probe",
                    sys.executable,
                    "-c",
                    replay_probe,
                ],
                stdout=subprocess.DEVNULL,
                env=env,
            )
            wait_for_output(
                [
                    cube,
                    "--manager-socket",
                    socket_path,
                    "--workspace",
                    "Project A",
                    "logs",
                    "--stdout",
                    "replay-probe",
                ],
                "\x1b[6n",
                env,
            )

            replay_master_fd, replay_slave_fd = pty.openpty()
            set_window_size(replay_master_fd, 24, 80)
            replay_connect = subprocess.Popen(
                [
                    cube,
                    "--manager-socket",
                    socket_path,
                    "--workspace",
                    "Project A",
                    "connect",
                    "replay-probe",
                ],
                stdin=replay_slave_fd,
                stdout=replay_slave_fd,
                stderr=replay_slave_fd,
                env=env,
                close_fds=True,
            )
            os.close(replay_slave_fd)
            try:
                read_until(replay_master_fd, b"NO_INPUT")
                time.sleep(0.1)
                os.write(replay_master_fd, b"\x1cd")
                replay_connect.wait(timeout=5)
            finally:
                if replay_connect.poll() is None:
                    replay_connect.terminate()
                    replay_connect.wait(timeout=5)
                os.close(replay_master_fd)

            probe_logs = wait_for_output(
                [
                    cube,
                    "--manager-socket",
                    socket_path,
                    "--workspace",
                    "Project A",
                    "logs",
                    "--stdout",
                    "replay-probe",
                ],
                "NO_INPUT",
                env,
            )
            if "GOT_INPUT" in probe_logs:
                raise AssertionError(f"replayed terminal response was forwarded: {probe_logs!r}")
            check_call(
                [
                    cube,
                    "--manager-socket",
                    socket_path,
                    "--workspace",
                    "Project A",
                    "kill",
                    "--cleanup",
                    "replay-probe",
                ],
                stdout=subprocess.DEVNULL,
                env=env,
            )
        finally:
            if cube_process is not None and cube_process.poll() is None:
                cube_process.send_signal(signal.SIGTERM)
                cube_process.wait(timeout=5)
            if master_fd >= 0:
                os.close(master_fd)
            if os.path.exists(socket_path):
                subprocess.run(
                    [sys.executable, api_client, socket_path, "shutdown"],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    check=False,
                )
            if manager_process.poll() is None:
                manager_process.terminate()
                manager_process.wait(timeout=5)
            if manager_process.returncode not in (0, -signal.SIGTERM, None):
                stderr = manager_process.stderr.read() if manager_process.stderr else ""
                raise AssertionError(f"manager exited {manager_process.returncode}: {stderr}")
            shutil.rmtree(xdg_state_home, ignore_errors=True)


if __name__ == "__main__":
    main()
