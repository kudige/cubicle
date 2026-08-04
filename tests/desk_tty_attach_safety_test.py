#!/usr/bin/env python3
import os
import pty
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import time


def wait_for_socket(path):
    deadline = time.time() + 5
    while time.time() < deadline:
        if os.path.exists(path):
            return
        time.sleep(0.05)
    raise AssertionError("manager socket was not created")


def run_checked(command, env, **kwargs):
    result = subprocess.run(
        command,
        env=env,
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


def run_desk_and_ctrl_c(desk, cube, env):
    master_fd, slave_fd = pty.openpty()
    proc = subprocess.Popen(
        [desk],
        stdin=slave_fd,
        stdout=slave_fd,
        stderr=subprocess.PIPE,
        env=env,
        close_fds=True,
    )
    os.close(slave_fd)
    os.write(master_fd, b"\x1b[12;34R")
    captured = bytearray()
    sent_ctrl_c = False
    deadline = time.time() + 5
    try:
        while time.time() < deadline:
            fds = [master_fd]
            if proc.stderr is not None:
                fds.append(proc.stderr.fileno())
            readable, _, _ = select.select(fds, [], [], 0.05)
            for fd in readable:
                if fd == master_fd:
                    try:
                        chunk = os.read(master_fd, 4096)
                    except OSError:
                        chunk = b""
                    captured.extend(chunk)
                elif proc.stderr is not None:
                    os.read(proc.stderr.fileno(), 4096)

            if not sent_ctrl_c and b"desk-safe" in captured:
                ps_result = subprocess.run(
                    [cube, "ps"],
                    env=env,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    timeout=2,
                )
                if ps_result.returncode != 0:
                    raise AssertionError(
                        f"cube ps failed while desk was open:\n"
                        f"stdout:\n{ps_result.stdout}\n"
                        f"stderr:\n{ps_result.stderr}"
                    )
                if "desk-safe\ttty\trunning" not in ps_result.stdout:
                    raise AssertionError(
                        f"cube ps did not see running cube while desk was open:\n"
                        f"{ps_result.stdout}"
                    )
                os.write(master_fd, b"\x03")
                sent_ctrl_c = True

            if proc.poll() is not None:
                break

        if not sent_ctrl_c:
            raise AssertionError(f"desk did not render attached pane: {captured!r}")
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        if proc.returncode != 0:
            raise AssertionError(
                f"desk exited with {proc.returncode}; output={captured!r}"
            )
    finally:
        os.close(master_fd)


def main():
    manager = os.environ["CUBICLE_MANAGER"]
    controller = os.environ["CUBICLE_CONTROLLER"]
    cube = os.environ["CUBE"]
    desk = os.environ["DESK"]
    api_client = os.environ["CUBICLE_API_CLIENT"]

    tmpdir = tempfile.mkdtemp(prefix="cubicle-desk-safety.")
    manager_proc = None
    try:
        state_dir = os.path.join(tmpdir, "manager-state")
        log_dir = os.path.join(tmpdir, "log")
        socket_path = os.path.join(tmpdir, "manager.sock")
        env = os.environ.copy()
        env["XDG_STATE_HOME"] = os.path.join(tmpdir, "xdg-state")
        env["XDG_RUNTIME_DIR"] = os.path.join(tmpdir, "runtime")
        env["XDG_CONFIG_HOME"] = os.path.join(tmpdir, "config")
        env["CUBICLE_MANAGER_SOCKET"] = socket_path
        os.makedirs(env["XDG_STATE_HOME"], exist_ok=True)
        os.makedirs(env["XDG_RUNTIME_DIR"], exist_ok=True)
        os.makedirs(env["XDG_CONFIG_HOME"], exist_ok=True)

        manager_proc = subprocess.Popen(
            [
                manager,
                "--state-dir",
                state_dir,
                "--log-dir",
                log_dir,
                "--controller-bin",
                controller,
                "daemon",
                "--foreground",
                "--control-socket",
                socket_path,
                "--event-interval-ms",
                "50",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env,
        )
        wait_for_socket(socket_path)

        for _ in range(100):
            ping = subprocess.run(
                [sys.executable, api_client, socket_path, "ping"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                env=env,
            )
            if ping.returncode == 0:
                break
            time.sleep(0.05)
        else:
            raise AssertionError("manager did not become ready")

        run_checked([cube, "workspace", "create", "DeskSafe"], env)
        run_checked([cube, "workspace", "DeskSafe"], env)
        run_checked(
            [
                cube,
                "run",
                "--bg",
                "--tty",
                "--name",
                "desk-safe",
                "sh",
                "-c",
                "echo READY; while true; do sleep 1; done",
            ],
            env,
        )

        run_desk_and_ctrl_c(desk, cube, env)

        ps_output = run_checked([cube, "ps"], env)
        if "desk-safe\ttty\trunning" not in ps_output:
            raise AssertionError(f"TTY process did not stay running:\n{ps_output}")

        event_logs = []
        controller_log_root = os.path.join(log_dir, "controllers")
        for root, _, files in os.walk(controller_log_root):
            if "events.log" in files:
                event_logs.append(os.path.join(root, "events.log"))
        if len(event_logs) != 1:
            raise AssertionError(f"expected one controller event log, got {event_logs}")
        with open(event_logs[0], "r", encoding="utf-8") as handle:
            events = handle.read()
        if "type=input" in events:
            raise AssertionError(f"desk startup/Ctrl-C forwarded input:\n{events}")

        run_checked([cube, "kill", "--all", "--cleanup"], env)
    finally:
        if manager_proc is not None:
            subprocess.run(
                [sys.executable, api_client, os.path.join(tmpdir, "manager.sock"), "shutdown"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                env=env if "env" in locals() else os.environ.copy(),
            )
            try:
                manager_proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                manager_proc.send_signal(signal.SIGTERM)
                try:
                    manager_proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    manager_proc.kill()
                    manager_proc.wait()
        shutil.rmtree(tmpdir, ignore_errors=True)


if __name__ == "__main__":
    main()
