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
import fcntl
import struct
import termios


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


def read_controller_events(log_dir):
    events = []
    controller_log_root = os.path.join(log_dir, "controllers")
    for root, _, files in os.walk(controller_log_root):
        if "events.log" in files:
            path = os.path.join(root, "events.log")
            with open(path, "r", encoding="utf-8") as handle:
                events.append(handle.read())
    return "\n".join(events)


def run_desk_and_ctrl_c(desk, cube, env, log_dir):
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
    sent_arrow = False
    saw_arrow = False
    sent_ctrl_c = False
    saw_ctrl_c = False
    sent_quit = False
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

            if not sent_arrow and b"desk-safe" in captured:
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
                os.write(master_fd, b"\x1b[A")
                sent_arrow = True

            if sent_arrow and not saw_arrow:
                events = read_controller_events(log_dir)
                if "type=input length=3" in events and "data_hex=1b5b41" in events:
                    saw_arrow = True

            if saw_arrow and not sent_ctrl_c:
                os.write(master_fd, b"\x03")
                sent_ctrl_c = True

            if sent_ctrl_c and not saw_ctrl_c:
                logs = subprocess.run(
                    [cube, "logs", "--stdout", "desk-safe"],
                    env=env,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                if "GOT_CTRL_C" in logs.stdout:
                    saw_ctrl_c = True

            if saw_ctrl_c and not sent_quit:
                if proc.poll() is not None:
                    raise AssertionError("desk exited after forwarding Ctrl-C")
                os.write(master_fd, b"\x18q")
                sent_quit = True

            if proc.poll() is not None:
                break

        if not sent_arrow:
            raise AssertionError(f"desk did not render attached pane: {captured!r}")
        if not saw_arrow:
            events = read_controller_events(log_dir)
            raise AssertionError(
                f"desk did not forward Up arrow as one CSI input event:\n{events}"
            )
        if not sent_ctrl_c:
            raise AssertionError("desk did not send Ctrl-C after Up arrow")
        if not saw_ctrl_c:
            raise AssertionError(f"desk did not forward Ctrl-C: {captured!r}")
        if not sent_quit:
            raise AssertionError("desk was not asked to quit")
        if proc.poll() is None:
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


def run_desk_until_command_output(desk, cube, env, process, marker):
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
    captured = bytearray()
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
                        captured.extend(os.read(master_fd, 4096))
                    except OSError:
                        pass
                elif proc.stderr is not None:
                    os.read(proc.stderr.fileno(), 4096)

            logs = subprocess.run(
                [cube, "logs", "--stdout", process],
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            if marker in logs.stdout:
                proc.terminate()
                proc.wait(timeout=2)
                if proc.returncode not in (0, -signal.SIGTERM):
                    raise AssertionError(
                        f"desk exited with {proc.returncode}; output={captured!r}"
                    )
                return

            if proc.poll() is not None:
                break

        raise AssertionError(
            f"desk did not produce {marker!r} for {process}; output={captured!r}"
        )
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        os.close(master_fd)


def run_desk_until_screen_marker(desk, env, marker):
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
    captured = bytearray()
    sent_quit = False
    marker_bytes = marker.encode("utf-8")
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
                        captured.extend(os.read(master_fd, 8192))
                    except OSError:
                        pass
                elif proc.stderr is not None:
                    os.read(proc.stderr.fileno(), 4096)

            if marker_bytes in captured and not sent_quit:
                os.write(master_fd, b"\x18q")
                sent_quit = True

            if proc.poll() is not None:
                break

        if not sent_quit:
            raise AssertionError(
                f"desk did not render marker {marker!r}: {captured!r}"
            )
        if proc.poll() is None:
            proc.wait(timeout=2)
        if proc.returncode != 0:
            raise AssertionError(
                f"desk exited with {proc.returncode}; output={captured!r}"
            )
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        os.close(master_fd)


def assert_desk_snapshot_skipped_backlog(log_path, process):
    with open(log_path, "r", encoding="utf-8") as handle:
        lines = handle.readlines()

    snapshot_line = None
    for line in lines:
        if f"event=snapshot_reload_ok process={process} " in line:
            snapshot_line = line
    if snapshot_line is None:
        raise AssertionError(f"missing snapshot log for {process}")

    fields = {}
    for part in snapshot_line.split():
        if "=" in part:
            key, value = part.split("=", 1)
            fields[key] = value
    offset = int(fields.get("offset", "0"))
    read_offset_after = int(fields.get("read_offset_after", "-1"))
    if offset < 4096 or read_offset_after != offset:
        raise AssertionError(
            f"snapshot did not advance read offset: {snapshot_line}"
        )

    read_lines = [
        line for line in lines
        if f"event=read_ok " in line and f"process={process} " in line
    ]
    if read_lines:
        raise AssertionError(
            f"desk read historical backlog after snapshot:\n{''.join(read_lines)}"
        )


def run_desk_with_terminal_noise(desk, cube, env):
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
    captured = bytearray()
    sent_noise = False
    sent_quit = False
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
                        captured.extend(os.read(master_fd, 4096))
                    except OSError:
                        pass
                elif proc.stderr is not None:
                    os.read(proc.stderr.fileno(), 4096)

            if not sent_noise and b"terminal-noise" in captured:
                os.write(
                    master_fd,
                    b"\x1b[I"
                    b"\x1b[10;1R"
                    b"\x1b]10;rgb:e3e3/e3e3/eaea\x1b\\"
                    b"\x1b]11;rgb:0808/0505/2b2b\x1b\\"
                    b"\x1b[?1;2;4c",
                )
                sent_noise = True

            if sent_noise and not sent_quit:
                time.sleep(0.2)
                os.write(master_fd, b"\x18q")
                sent_quit = True

            if proc.poll() is not None:
                break

        if not sent_noise:
            raise AssertionError(
                f"desk did not render terminal-noise pane: {captured!r}"
            )
        if proc.poll() is None:
            proc.wait(timeout=2)
        if proc.returncode != 0:
            raise AssertionError(
                f"desk exited with {proc.returncode}; output={captured!r}"
            )
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        os.close(master_fd)


def run_desk_until_emacs_render(desk, env):
    master_fd, slave_fd = pty.openpty()
    fcntl.ioctl(slave_fd, termios.TIOCSWINSZ,
                struct.pack("HHHH", 30, 100, 0, 0))
    proc = subprocess.Popen(
        [desk],
        stdin=slave_fd,
        stdout=slave_fd,
        stderr=subprocess.PIPE,
        env=env,
        close_fds=True,
    )
    os.close(slave_fd)
    captured = bytearray()
    sent_quit = False
    deadline = time.time() + 8
    try:
        while time.time() < deadline:
            fds = [master_fd]
            if proc.stderr is not None:
                fds.append(proc.stderr.fileno())
            readable, _, _ = select.select(fds, [], [], 0.05)
            for fd in readable:
                if fd == master_fd:
                    try:
                        captured.extend(os.read(master_fd, 8192))
                    except OSError:
                        pass
                elif proc.stderr is not None:
                    os.read(proc.stderr.fileno(), 4096)

            if (b"line one visible" in captured and
                    b"\x1b[28;1H" in captured and
                    b"sample.txt" in captured and
                    not sent_quit):
                os.write(master_fd, b"\x18q")
                sent_quit = True

            if proc.poll() is not None:
                break

        if not sent_quit:
            raise AssertionError(
                "desk did not render Emacs snapshot at pane height; "
                f"output={captured!r}"
            )
        if proc.poll() is None:
            proc.wait(timeout=2)
        if proc.returncode != 0:
            raise AssertionError(
                f"desk exited with {proc.returncode}; output={captured!r}"
            )
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        os.close(master_fd)


def run_desk_echo_latency(desk, env):
    master_fd, slave_fd = pty.openpty()
    fcntl.ioctl(slave_fd, termios.TIOCSWINSZ,
                struct.pack("HHHH", 24, 80, 0, 0))
    proc = subprocess.Popen(
        [desk],
        stdin=slave_fd,
        stdout=slave_fd,
        stderr=subprocess.PIPE,
        env=env,
        close_fds=True,
    )
    os.close(slave_fd)
    captured = bytearray()
    sent_input = False
    sent_at = 0.0
    saw_echo = False
    sent_quit = False
    deadline = time.time() + 6
    try:
        while time.time() < deadline:
            fds = [master_fd]
            if proc.stderr is not None:
                fds.append(proc.stderr.fileno())
            readable, _, _ = select.select(fds, [], [], 0.02)
            for fd in readable:
                if fd == master_fd:
                    try:
                        captured.extend(os.read(master_fd, 8192))
                    except OSError:
                        pass
                elif proc.stderr is not None:
                    os.read(proc.stderr.fileno(), 4096)

            if not sent_input and b"echo-latency" in captured:
                sent_at = time.time()
                os.write(master_fd, b"abc")
                sent_input = True

            if sent_input and not saw_echo and b"ECHO:abc" in captured:
                if time.time() - sent_at > 1.0:
                    raise AssertionError(
                        f"desk echo was too slow; output={captured!r}"
                    )
                saw_echo = True
                os.write(master_fd, b"\x18q")
                sent_quit = True

            if proc.poll() is not None:
                break

        if not sent_input:
            raise AssertionError(
                f"desk did not render echo-latency pane: {captured!r}"
            )
        if not saw_echo:
            raise AssertionError(f"desk did not render echoed input: {captured!r}")
        if not sent_quit:
            raise AssertionError("desk was not asked to quit")
        if proc.poll() is None:
            proc.wait(timeout=2)
        if proc.returncode != 0:
            raise AssertionError(
                f"desk exited with {proc.returncode}; output={captured!r}"
            )
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        os.close(master_fd)


def run_desk_cursor_overlay(desk, env, expect_cursor):
    master_fd, slave_fd = pty.openpty()
    fcntl.ioctl(slave_fd, termios.TIOCSWINSZ,
                struct.pack("HHHH", 24, 80, 0, 0))
    proc = subprocess.Popen(
        [desk],
        stdin=slave_fd,
        stdout=slave_fd,
        stderr=subprocess.PIPE,
        env=env,
        close_fds=True,
    )
    os.close(slave_fd)
    captured = bytearray()
    sent_quit = False
    deadline = time.time() + 5
    cursor_draw = b"\x1b[2;15H\x1b[0m\x1b[7m"
    try:
        while time.time() < deadline:
            fds = [master_fd]
            if proc.stderr is not None:
                fds.append(proc.stderr.fileno())
            readable, _, _ = select.select(fds, [], [], 0.05)
            for fd in readable:
                if fd == master_fd:
                    try:
                        captured.extend(os.read(master_fd, 8192))
                    except OSError:
                        pass
                elif proc.stderr is not None:
                    os.read(proc.stderr.fileno(), 4096)

            if b"cursor-probe" in captured:
                if expect_cursor and cursor_draw in captured and not sent_quit:
                    os.write(master_fd, b"\x18q")
                    sent_quit = True
                elif not expect_cursor and time.time() > deadline - 1.0:
                    os.write(master_fd, b"\x18q")
                    sent_quit = True

            if proc.poll() is not None:
                break

        if not sent_quit:
            expectation = "draw cursor" if expect_cursor else "hide cursor"
            raise AssertionError(
                f"desk did not {expectation}; output={captured!r}"
            )
        if expect_cursor and cursor_draw not in captured:
            raise AssertionError(
                f"desk did not draw block cursor at expected cell: {captured!r}"
            )
        if not expect_cursor and cursor_draw in captured:
            raise AssertionError(
                f"desk drew cursor after app hid it: {captured!r}"
            )
        if proc.poll() is None:
            proc.wait(timeout=2)
        if proc.returncode != 0:
            raise AssertionError(
                f"desk exited with {proc.returncode}; output={captured!r}"
            )
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
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
        os.makedirs(os.path.join(env["XDG_CONFIG_HOME"], "cubicle"), exist_ok=True)
        with open(
            os.path.join(env["XDG_CONFIG_HOME"], "cubicle", "config.cfg"),
            "w",
            encoding="utf-8",
        ) as handle:
            handle.write(
                "[manager]\n"
                f"log_dir={log_dir}\n"
                "\n"
                "[controller]\n"
                "debug=input\n"
                "\n"
                "[desk]\n"
                "debug=library,terminal\n"
            )

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
                sys.executable,
                "-c",
                (
                    "import os,select,sys,time,tty\n"
                    "tty.setraw(0)\n"
                    "sys.stdout.write('READY\\n')\n"
                    "sys.stdout.flush()\n"
                    "deadline=time.time()+10\n"
                    "while time.time()<deadline:\n"
                    "    r,_,_=select.select([sys.stdin],[],[],0.05)\n"
                    "    if r and b'\\x03' in os.read(0,64):\n"
                    "        sys.stdout.write('GOT_CTRL_C\\n')\n"
                    "        sys.stdout.flush()\n"
                    "        break\n"
                    "time.sleep(10)\n"
                ),
            ],
            env,
        )

        run_desk_and_ctrl_c(desk, cube, env, log_dir)
        library_log = os.path.join(log_dir, "client-library.log")
        if not os.path.exists(library_log):
            raise AssertionError("desk.debug=library did not create client library log")
        with open(library_log, "r", encoding="utf-8") as handle:
            library_events = handle.read()
        if "program=desk " not in library_events:
            raise AssertionError(f"desk library log missing program marker:\n{library_events}")
        if "event=rpc.request method=workspace.get code=ok" not in library_events:
            raise AssertionError(f"desk library log missing workspace RPC:\n{library_events}")
        if "event=rpc.request method=controller.read code=ok" not in library_events:
            raise AssertionError(f"desk library log missing controller read RPC:\n{library_events}")
        desk_terminal_log = os.path.join(log_dir, "desk-terminal.log")
        if not os.path.exists(desk_terminal_log):
            raise AssertionError("desk.debug=terminal did not create desk terminal log")
        with open(desk_terminal_log, "r", encoding="utf-8") as handle:
            desk_terminal_events = handle.read()
        if "event=loop_start" not in desk_terminal_events:
            raise AssertionError(
                f"desk terminal log missing loop_start:\n{desk_terminal_events}"
            )

        ps_output = run_checked([cube, "ps"], env)
        if "desk-safe\ttty\trunning" not in ps_output:
            raise AssertionError(f"TTY process did not stay running:\n{ps_output}")

        events = read_controller_events(log_dir)
        if "type=input length=1" not in events:
            raise AssertionError(f"desk did not record forwarded Ctrl-C:\n{events}")

        run_checked([cube, "kill", "--all", "--cleanup"], env)
        run_checked(
            [
                cube,
                "run",
                "--bg",
                "--tty",
                "--name",
                "backlog-snapshot",
                sys.executable,
                "-c",
                (
                    "import sys,time\n"
                    "for i in range(900):\n"
                    "    sys.stdout.write('backlog line %04d - snowman \\u2603\\n' % i)\n"
                    "sys.stdout.write('BACKLOG_READY\\n')\n"
                    "sys.stdout.flush()\n"
                    "time.sleep(8)\n"
                ),
            ],
            env,
        )
        deadline = time.time() + 5
        while time.time() < deadline:
            logs = subprocess.run(
                [cube, "logs", "--stdout", "backlog-snapshot"],
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            if "BACKLOG_READY" in logs.stdout:
                break
            time.sleep(0.05)
        else:
            raise AssertionError("backlog process did not finish initial output")
        run_desk_until_screen_marker(desk, env, "BACKLOG_READY")
        assert_desk_snapshot_skipped_backlog(
            desk_terminal_log, "backlog-snapshot"
        )

        run_checked([cube, "kill", "--all", "--cleanup"], env)
        run_checked(
            [
                cube,
                "run",
                "--bg",
                "--tty",
                "--name",
                "dsr-probe",
                sys.executable,
                "-c",
                (
                    "import os,select,sys,time,tty\n"
                    "tty.setraw(0)\n"
                    "time.sleep(0.5)\n"
                    "sys.stdout.write('\\x1b[6n')\n"
                    "sys.stdout.flush()\n"
                    "deadline=time.time()+3\n"
                    "data=b''\n"
                    "while time.time()<deadline:\n"
                    "    r,_,_=select.select([sys.stdin],[],[],0.05)\n"
                    "    if r:\n"
                    "        data+=os.read(0,64)\n"
                    "        if data.endswith(b'R'):\n"
                    "            break\n"
                    "sys.stdout.write('GOT_DSR\\n' if data.startswith(b'\\x1b[') and data.endswith(b'R') else 'NO_DSR\\n')\n"
                    "sys.stdout.flush()\n"
                    "time.sleep(5)\n"
                ),
            ],
            env,
        )
        run_desk_until_command_output(desk, cube, env, "dsr-probe", "GOT_DSR")

        run_checked([cube, "kill", "--all", "--cleanup"], env)
        run_checked(
            [
                cube,
                "run",
                "--bg",
                "--tty",
                "--name",
                "dsr-single-response",
                sys.executable,
                "-c",
                (
                    "import os,select,sys,time,tty\n"
                    "tty.setraw(0)\n"
                    "time.sleep(0.5)\n"
                    "sys.stdout.write('\\x1b[6n')\n"
                    "sys.stdout.flush()\n"
                    "deadline=time.time()+1\n"
                    "data=b''\n"
                    "while time.time()<deadline:\n"
                    "    r,_,_=select.select([sys.stdin],[],[],0.05)\n"
                    "    if r:\n"
                    "        data+=os.read(0,256)\n"
                    "sys.stdout.write('DSR_COUNT:%d\\n' % data.count(b'R'))\n"
                    "sys.stdout.flush()\n"
                    "time.sleep(3)\n"
                ),
            ],
            env,
        )
        run_desk_until_command_output(
            desk, cube, env, "dsr-single-response", "DSR_COUNT:1"
        )

        run_checked([cube, "kill", "--all", "--cleanup"], env)
        run_checked(
            [
                cube,
                "run",
                "--bg",
                "--tty",
                "--name",
                "terminal-noise",
                sys.executable,
                "-c",
                (
                    "import os,select,sys,time,tty\n"
                    "tty.setraw(0)\n"
                    "sys.stdout.write('READY terminal-noise\\n')\n"
                    "sys.stdout.flush()\n"
                    "deadline=time.time()+3\n"
                    "while time.time()<deadline:\n"
                    "    r,_,_=select.select([sys.stdin],[],[],0.05)\n"
                    "    if r:\n"
                    "        data=os.read(0,256)\n"
                    "        if data:\n"
                    "            sys.stdout.write('GOT_INPUT\\n')\n"
                    "            sys.stdout.flush()\n"
                    "            break\n"
                    "time.sleep(3)\n"
                ),
            ],
            env,
        )
        run_desk_with_terminal_noise(desk, cube, env)
        noise_logs = subprocess.run(
            [cube, "logs", "--stdout", "terminal-noise"],
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if "GOT_INPUT" in noise_logs.stdout:
            raise AssertionError(
                f"desk forwarded terminal response bytes:\n{noise_logs.stdout}"
            )

        run_checked([cube, "kill", "--all", "--cleanup"], env)
        run_checked(
            [
                cube,
                "run",
                "--bg",
                "--tty",
                "--name",
                "echo-latency",
                sys.executable,
                "-c",
                (
                    "import os,select,sys,time,tty\n"
                    "tty.setraw(0)\n"
                    "sys.stdout.write('READY echo-latency\\n')\n"
                    "sys.stdout.flush()\n"
                    "deadline=time.time()+8\n"
                    "data=b''\n"
                    "while time.time()<deadline:\n"
                    "    r,_,_=select.select([sys.stdin],[],[],0.02)\n"
                    "    if r:\n"
                    "        data+=os.read(0,64)\n"
                    "        if b'abc' in data:\n"
                    "            sys.stdout.write('ECHO:abc\\n')\n"
                    "            sys.stdout.flush()\n"
                    "            break\n"
                    "time.sleep(5)\n"
                ),
            ],
            env,
        )
        run_desk_echo_latency(desk, env)

        run_checked([cube, "kill", "--all", "--cleanup"], env)
        run_checked(
            [
                cube,
                "run",
                "--bg",
                "--tty",
                "--name",
                "cursor-visible",
                sys.executable,
                "-c",
                (
                    "import sys,time,tty\n"
                    "tty.setraw(0)\n"
                    "sys.stdout.write('\\x1b[2J\\x1b[2;3Hcursor-probe')\n"
                    "sys.stdout.flush()\n"
                    "time.sleep(5)\n"
                ),
            ],
            env,
        )
        run_desk_cursor_overlay(desk, env, True)

        run_checked([cube, "kill", "--all", "--cleanup"], env)
        run_checked(
            [
                cube,
                "run",
                "--bg",
                "--tty",
                "--name",
                "cursor-hidden",
                sys.executable,
                "-c",
                (
                    "import sys,time,tty\n"
                    "tty.setraw(0)\n"
                    "sys.stdout.write('\\x1b[2J\\x1b[?25l\\x1b[2;3Hcursor-probe')\n"
                    "sys.stdout.flush()\n"
                    "time.sleep(5)\n"
                ),
            ],
            env,
        )
        run_desk_cursor_overlay(desk, env, False)

        run_checked([cube, "kill", "--all", "--cleanup"], env)
        emacs = shutil.which("emacs")
        if emacs is not None:
            sample_path = os.path.join(tmpdir, "sample.txt")
            with open(sample_path, "w", encoding="utf-8") as handle:
                handle.write(
                    "line one visible\n"
                    "line two visible\n"
                    "line three visible\n"
                )
            run_checked(
                [
                    cube,
                    "run",
                    "--bg",
                    "--tty",
                    "--name",
                    "emacs-file",
                    emacs,
                    "-nw",
                    "-Q",
                    sample_path,
                ],
                env,
            )
            run_desk_until_emacs_render(desk, env)

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
