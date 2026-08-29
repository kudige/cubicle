#!/usr/bin/env python3
import os
import pty
import re
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import fcntl
import json
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


def write_test_config(config_path, log_dir, extra=""):
    with open(config_path, "w", encoding="utf-8") as handle:
        handle.write(
            "[manager]\n"
            f"log_dir={log_dir}\n"
            "\n"
            "[controller]\n"
            "debug=input\n"
            "\n"
            "[desk]\n"
            "debug=library,terminal\n"
            f"{extra}"
        )


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
    sent_split_arrow = False
    saw_split_arrow = False
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

            if saw_arrow and not sent_split_arrow:
                os.write(master_fd, b"\x1b")
                time.sleep(0.01)
                os.write(master_fd, b"[B")
                sent_split_arrow = True

            if sent_split_arrow and not saw_split_arrow:
                events = read_controller_events(log_dir)
                if "type=input length=3" in events and "data_hex=1b5b42" in events:
                    saw_split_arrow = True

            if saw_split_arrow and not sent_ctrl_c:
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
        if not sent_split_arrow:
            raise AssertionError("desk did not send split Down arrow")
        if not saw_split_arrow:
            events = read_controller_events(log_dir)
            raise AssertionError(
                f"desk did not reassemble split Down arrow as one CSI input event:\n{events}"
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


def run_desk_survives_dead_pane(desk, cube, env):
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
    saw_dying_pane = False
    saw_surviving_pane = False
    saw_ended_notice = False
    sent_payload = False
    saw_payload = False
    sent_quit = False
    deadline = time.time() + 7
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

            if b"desk-dies" in captured or b"READY_DIES" in captured:
                saw_dying_pane = True
            if b"desk-safe" in captured:
                saw_surviving_pane = True
            if b"desk-dies just ended" in captured:
                saw_ended_notice = True

            if saw_dying_pane and saw_surviving_pane and saw_ended_notice and not sent_payload:
                time.sleep(0.8)
                if proc.poll() is not None:
                    break
                os.write(master_fd, b"PING")
                sent_payload = True

            if sent_payload and not saw_payload:
                logs = subprocess.run(
                    [cube, "logs", "--stdout", "desk-safe"],
                    env=env,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                if "GOT_PING" in logs.stdout:
                    saw_payload = True

            if saw_payload and not sent_quit:
                os.write(master_fd, b"\x18q")
                sent_quit = True

            if proc.poll() is not None:
                break

        if not saw_dying_pane:
            raise AssertionError(f"desk did not render dying pane: {captured!r}")
        if not saw_surviving_pane:
            raise AssertionError(f"desk did not render surviving pane: {captured!r}")
        if not saw_ended_notice:
            raise AssertionError(f"desk did not show ended pane notice: {captured!r}")
        if not sent_payload:
            raise AssertionError("desk exited before testing surviving pane input")
        if not saw_payload:
            raise AssertionError(
                f"desk did not keep surviving pane attached: {captured!r}"
            )
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


def find_saved_layout_file(env, name):
    return os.path.join(
        env["XDG_STATE_HOME"], "cubicle", "desk-layouts", "named",
        f"{name}.layout",
    )


def find_workspace_id(cube, env, name):
    output = run_checked([cube, "--json", "workspace", "list"], env)
    data = json.loads(output)
    for workspace in data.get("workspaces", []):
        if workspace.get("name") == name:
            return workspace.get("id")
    raise AssertionError(f"workspace {name!r} not found in {output}")


def workspace_layout_file(env, workspace_id):
    return os.path.join(
        env["XDG_STATE_HOME"], "cubicle", "desk-layouts",
        f"{workspace_id}.layout",
    )


def seed_workspace_macro(
    state_dir,
    workspace_id,
    ordinal,
    name,
    text,
    target_process_name="",
    key_name="",
):
    path = os.path.join(state_dir, "workspace-macros.tsv")
    with open(path, "a", encoding="utf-8") as handle:
        handle.write(
            f"{workspace_id}\t{ordinal}\t{name}\t{text}\t0\t"
            f"{target_process_name}\t{key_name}\n"
        )


def desk_default_workspace_file(env):
    return os.path.join(env["XDG_STATE_HOME"], "cubicle", "desk-workspace")


def desk_default_layout_file(env):
    return os.path.join(env["XDG_STATE_HOME"], "cubicle", "desk-layout")


def clear_desk_defaults(env):
    for path in (desk_default_workspace_file(env), desk_default_layout_file(env)):
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass


def run_desk_three_pane_default_layout(desk, cube, env):
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
    step = 0
    saw_bottom = False
    saw_top = False
    saw_right = False
    saw_left = False
    saw_full_zoom_redraw = False
    saw_full_zoom_restore = False
    saw_vertical_zoom_redraw = False
    saw_vertical_zoom_preserved = False
    sent_vertical_switch_at = 0.0
    deadline = time.time() + 6
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

            if (
                b"three-top" in captured
                and b"three-bottom" in captured
                and b"three-right" in captured
                and step == 0
            ):
                os.write(master_fd, b"\x18\x1b[BHIT_BOTTOM")
                step = 1

            if step == 1 and not saw_bottom:
                logs = subprocess.run(
                    [cube, "--workspace", "DeskThree", "logs", "--stdout", "three-bottom"],
                    env=env,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                if "GOT three-bottom HIT_BOTTOM" in logs.stdout:
                    saw_bottom = True
                    os.write(master_fd, b"\x18\x1b[AHIT_TOP")
                    step = 2

            if step == 2 and not saw_top:
                logs = subprocess.run(
                    [cube, "--workspace", "DeskThree", "logs", "--stdout", "three-top"],
                    env=env,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                if "GOT three-top HIT_TOP" in logs.stdout:
                    saw_top = True
                    os.write(master_fd, b"\x18\x1b[CHIT_RIGHT")
                    step = 3

            if step == 3 and not saw_right:
                logs = subprocess.run(
                    [cube, "--workspace", "DeskThree", "logs", "--stdout", "three-right"],
                    env=env,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                if "GOT three-right HIT_RIGHT" in logs.stdout:
                    saw_right = True
                    os.write(master_fd, b"\x18\x1b[DHIT_LEFT")
                    step = 4

            if step == 4 and not saw_left:
                top_logs = subprocess.run(
                    [cube, "--workspace", "DeskThree", "logs", "--stdout", "three-top"],
                    env=env,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                bottom_logs = subprocess.run(
                    [cube, "--workspace", "DeskThree", "logs", "--stdout", "three-bottom"],
                    env=env,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                if (
                    "GOT three-top HIT_LEFT" in top_logs.stdout
                    or "GOT three-bottom HIT_LEFT" in bottom_logs.stdout
                ):
                    saw_left = True
                    captured.clear()
                    os.write(master_fd, b"\x18 ")
                    step = 5

            if step == 5 and not saw_full_zoom_redraw:
                if b"\x1b[H\x1b[2J" in captured:
                    latest_frame = captured.split(b"\x1b[H\x1b[2J")[-1]
                    if (
                        b"three-top" in latest_frame
                        and b"three-bottom" not in latest_frame
                        and b"three-right" not in latest_frame
                    ):
                        saw_full_zoom_redraw = True
                        captured.clear()
                        os.write(master_fd, b"\x18 ")
                        step = 6

            if step == 6 and not saw_full_zoom_restore:
                if b"\x1b[H\x1b[2J" in captured:
                    latest_frame = captured.split(b"\x1b[H\x1b[2J")[-1]
                    if (
                        b"three-top" in latest_frame
                        and b"three-bottom" in latest_frame
                        and b"three-right" in latest_frame
                    ):
                        saw_full_zoom_restore = True
                        captured.clear()
                        os.write(master_fd, b"\x18sv")
                        step = 7

            if step == 7 and not saw_vertical_zoom_redraw:
                if b"\x1b[H\x1b[2J" in captured:
                    latest_frame = captured.split(b"\x1b[H\x1b[2J")[-1]
                    if (
                        b"three-top" in latest_frame
                        and b"three-right" in latest_frame
                        and b"three-bottom" not in latest_frame
                    ):
                        saw_vertical_zoom_redraw = True
                        captured.clear()
                        os.write(master_fd, b"q\x18n")
                        sent_vertical_switch_at = time.time()
                        step = 8

            if step == 8 and not saw_vertical_zoom_preserved:
                if (
                    sent_vertical_switch_at > 0
                    and b"\x1b[H\x1b[2J" in captured
                ):
                    latest_frame = captured.split(b"\x1b[H\x1b[2J")[-1]
                    if (
                        b"three-top" in latest_frame
                        and b"three-right" in latest_frame
                        and b"three-bottom" not in latest_frame
                    ):
                        saw_vertical_zoom_preserved = True
                        os.write(master_fd, b"\x18srq")
                        os.write(master_fd, b"\x18q")
                        sent_quit = True
                    elif (
                        time.time() - sent_vertical_switch_at > 0.3
                        and b"three-top" in latest_frame
                        and b"three-bottom" in latest_frame
                        and b"three-right" in latest_frame
                    ):
                        raise AssertionError(
                            f"desk reset vertical zoom while switching panes: {latest_frame!r}"
                        )

            if proc.poll() is not None:
                break

        if not sent_quit:
            raise AssertionError(
                f"desk did not complete directional pane checks: {captured!r}"
            )
        if not (saw_bottom and saw_top and saw_right and saw_left):
            raise AssertionError(
                "desk directional pane selection failed: "
                f"bottom={saw_bottom} top={saw_top} right={saw_right} "
                f"left={saw_left} output={captured!r}"
            )
        if not (saw_full_zoom_redraw and saw_full_zoom_restore):
            raise AssertionError(
                "desk full zoom did not redraw immediately: "
                f"redraw={saw_full_zoom_redraw} "
                f"restore={saw_full_zoom_restore} output={captured!r}"
            )
        if not (
            saw_vertical_zoom_redraw
            and saw_vertical_zoom_preserved
        ):
            raise AssertionError(
                "desk axis zoom did not redraw after hiding panes: "
                f"vertical={saw_vertical_zoom_redraw} "
                f"preserved={saw_vertical_zoom_preserved} "
                f"output={captured!r}"
            )
        if proc.poll() is None:
            proc.wait(timeout=2)
        if proc.returncode != 0:
            raise AssertionError(
                f"desk three-pane layout exited with {proc.returncode}; output={captured!r}"
            )

        workspace_id = find_workspace_id(cube, env, "DeskThree")
        path = workspace_layout_file(env, workspace_id)
        with open(path, "r", encoding="utf-8") as handle:
            layout = handle.read()
        node_lines = [line for line in layout.splitlines() if line.startswith("node ")]
        if len(node_lines) != 5:
            raise AssertionError(f"three-pane layout should have 5 nodes:\n{layout}")

        nodes = {}
        for line in node_lines:
            parts = line.split()
            index = int(parts[1])
            nodes[index] = {
                "pane_id": int(parts[2]),
                "split": int(parts[3]),
                "first": int(parts[4]),
                "second": int(parts[5]),
                "label": parts[7] if len(parts) > 7 else "",
            }
        root = int(next(line.split()[1] for line in layout.splitlines()
                       if line.startswith("root ")))
        root_node = nodes[root]
        if root_node["split"] != 1:
            raise AssertionError(f"root should split left/right:\n{layout}")
        left = nodes[root_node["first"]]
        right = nodes[root_node["second"]]
        if left["split"] != 2:
            raise AssertionError(f"left pane should split top/bottom:\n{layout}")
        top = nodes[left["first"]]
        bottom = nodes[left["second"]]
        if (
            top["pane_id"] != 1
            or top["label"] != "three-top"
            or bottom["pane_id"] != 2
            or bottom["label"] != "three-bottom"
            or right["pane_id"] != 3
            or right["label"] != "three-right"
        ):
            raise AssertionError(f"unexpected three-pane leaf assignment:\n{layout}")
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        os.close(master_fd)


def run_desk_save_and_load_layout(desk, env):
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
    sent_save = False
    saw_save_prompt = False
    sent_save_name = False
    saved_file = None
    sent_root_menu = False
    saw_root_layout = False
    closed_root_menu = False
    sent_picker = False
    saw_picker = False
    sent_filter = False
    sent_load = False
    sent_quit = False
    deadline = time.time() + 7
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

            if not sent_save and b"desk-safe" in captured:
                os.write(master_fd, b"\x18:")
                sent_save = True

            if sent_save and not saw_save_prompt and b"Save layout" in captured:
                saw_save_prompt = True

            if saw_save_prompt and not sent_save_name:
                os.write(master_fd, b"saved-alpha\r")
                sent_save_name = True

            if sent_save_name and saved_file is None:
                candidate = find_saved_layout_file(env, "saved-alpha")
                if os.path.exists(candidate):
                    saved_file = candidate

            if saved_file is not None and not sent_root_menu:
                with open(saved_file, "r", encoding="utf-8") as handle:
                    saved_content = handle.read()
                if "desk-named-layout-v1" not in saved_content:
                    raise AssertionError(
                        f"saved layout missing header:\n{saved_content}"
                    )
                if "pane 1 " not in saved_content:
                    raise AssertionError(
                        f"saved layout missing pane mapping:\n{saved_content}"
                    )
                captured.clear()
                os.write(master_fd, b"\x18o")
                sent_root_menu = True

            if sent_root_menu and not saw_root_layout:
                if b"Open cube" in captured and b"layout: saved-alpha" in captured:
                    saw_root_layout = True
                    os.write(master_fd, b"q")

            if saw_root_layout and not closed_root_menu:
                time.sleep(0.1)
                captured.clear()
                closed_root_menu = True

            if closed_root_menu and not sent_picker:
                captured.clear()
                os.write(master_fd, b"\x18;")
                sent_picker = True

            if sent_picker and not sent_filter and b"Load layout" in captured:
                os.write(master_fd, b"alpha")
                sent_filter = True

            if sent_picker and not saw_picker:
                if b"Load layout" in captured and b"Search: alpha" in captured:
                    saw_picker = True

            if saw_picker and not sent_load and b"saved-alpha" in captured:
                os.write(master_fd, b"\r")
                sent_load = True

            if sent_load and not sent_quit:
                time.sleep(0.2)
                if proc.poll() is not None:
                    break
                os.write(master_fd, b"\x18q")
                sent_quit = True

            if proc.poll() is not None:
                break

        if not sent_save:
            raise AssertionError(f"desk did not render initial pane: {captured!r}")
        if not saw_save_prompt:
            raise AssertionError(f"desk did not show save prompt: {captured!r}")
        if not sent_save_name:
            raise AssertionError("desk was not sent a layout name")
        if saved_file is None:
            raise AssertionError("desk did not save named layout")
        if not sent_root_menu:
            raise AssertionError("desk did not open root menu after saving layout")
        if not saw_root_layout:
            raise AssertionError(f"desk root menu did not show saved layout: {captured!r}")
        if not sent_filter:
            raise AssertionError(f"desk did not show layout picker: {captured!r}")
        if not saw_picker:
            raise AssertionError(f"desk did not show filtered layout picker: {captured!r}")
        if not sent_load:
            raise AssertionError(f"desk did not show saved layout entry: {captured!r}")
        if not sent_quit:
            raise AssertionError("desk was not asked to quit after loading layout")
        with open(desk_default_layout_file(env), "r", encoding="utf-8") as handle:
            if handle.read().strip() != "saved-alpha":
                raise AssertionError("desk did not persist loaded layout")
        if proc.poll() is None:
            proc.wait(timeout=2)
        if proc.returncode != 0:
            raise AssertionError(
                f"desk exited with {proc.returncode}; output={captured!r}"
            )
        clear_desk_defaults(env)
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        os.close(master_fd)


def run_desk_layout_mode_keys(desk, cube, env):
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
    sent_probe_start = False
    saw_probe_start = False
    sent_layout_keys = False
    sent_payload = False
    saw_payload = False
    sent_quit = False
    deadline = time.time() + 6
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

            if not sent_probe_start and b"desk-safe" in captured:
                os.write(master_fd, b"LEAK_START")
                sent_probe_start = True

            if sent_probe_start and not saw_probe_start:
                logs = subprocess.run(
                    [cube, "logs", "--stdout", "desk-safe"],
                    env=env,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                if "GOT_LEAK_START" in logs.stdout:
                    saw_probe_start = True

            if saw_probe_start and not sent_layout_keys:
                os.write(master_fd, b"\x18shvrtq")
                sent_layout_keys = True

            if sent_layout_keys and not sent_payload:
                time.sleep(0.2)
                logs = subprocess.run(
                    [cube, "logs", "--stdout", "desk-safe"],
                    env=env,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                if "GOT_LAYOUT_LEAK" in logs.stdout:
                    raise AssertionError(
                        f"layout-mode keys leaked into cube stdin:\n{logs.stdout}"
                    )
                os.write(master_fd, b"LAYOUT")
                sent_payload = True

            if sent_payload and not saw_payload:
                logs = subprocess.run(
                    [cube, "logs", "--stdout", "desk-safe"],
                    env=env,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                if "GOT_LAYOUT" in logs.stdout:
                    saw_payload = True

            if saw_payload and not sent_quit:
                os.write(master_fd, b"\x18q")
                sent_quit = True

            if proc.poll() is not None:
                break

        if not sent_probe_start:
            raise AssertionError(f"desk did not render layout-mode pane: {captured!r}")
        if not saw_probe_start:
            raise AssertionError("desk did not forward pre-layout probe")
        if not sent_layout_keys:
            raise AssertionError(f"desk did not render layout-mode pane: {captured!r}")
        if not sent_payload:
            raise AssertionError("desk did not send post-layout-mode payload")
        if not saw_payload:
            raise AssertionError(f"desk did not exit layout mode: {captured!r}")
        if proc.poll() is None:
            proc.wait(timeout=2)
        if proc.returncode != 0:
            raise AssertionError(
                f"desk layout mode exited with {proc.returncode}; output={captured!r}"
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


def run_desk_scroll_mode_keys(desk, cube, env):
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
    sent_probe_start = False
    saw_probe_start = False
    sent_scroll_keys = False
    sent_payload = False
    saw_payload = False
    sent_quit = False
    deadline = time.time() + 7
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

            if not sent_probe_start and b"desk-safe" in captured:
                os.write(master_fd, b"SCROLL_START")
                sent_probe_start = True

            if sent_probe_start and not saw_probe_start:
                logs = subprocess.run(
                    [cube, "logs", "--stdout", "desk-safe"],
                    env=env,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                if "GOT_SCROLL_START" in logs.stdout:
                    saw_probe_start = True

            if saw_probe_start and not sent_scroll_keys:
                os.write(
                    master_fd,
                    b"\x18\x1b[5~"
                    b"\x1b[A\x1b[B\x1b[C\x1b[D"
                    b"\x1b[5~\x1b[6~\x1b[H\x1b[F"
                    b"q",
                )
                sent_scroll_keys = True

            if sent_scroll_keys and not sent_payload:
                time.sleep(0.2)
                logs = subprocess.run(
                    [cube, "logs", "--stdout", "desk-safe"],
                    env=env,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                if "GOT_SCROLL_LEAK" in logs.stdout:
                    raise AssertionError(
                        f"scroll-mode keys leaked into cube stdin:\n{logs.stdout}"
                    )
                os.write(master_fd, b"SCROLL_DONE")
                sent_payload = True

            if sent_payload and not saw_payload:
                logs = subprocess.run(
                    [cube, "logs", "--stdout", "desk-safe"],
                    env=env,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                if "GOT_SCROLL_DONE" in logs.stdout:
                    saw_payload = True

            if saw_payload and not sent_quit:
                os.write(master_fd, b"\x18q")
                sent_quit = True

            if proc.poll() is not None:
                break

        if not sent_probe_start:
            raise AssertionError(f"desk did not render scroll-mode pane: {captured!r}")
        if not saw_probe_start:
            raise AssertionError(f"desk did not send scroll probe marker: {captured!r}")
        if not sent_scroll_keys:
            raise AssertionError("desk was not sent scroll-mode keys")
        if not sent_payload:
            raise AssertionError("desk did not send post-scroll-mode payload")
        if not saw_payload:
            raise AssertionError(f"desk did not exit scroll mode: {captured!r}")
        if proc.poll() is None:
            proc.wait(timeout=2)
        if proc.returncode != 0:
            raise AssertionError(
                f"desk scroll mode exited with {proc.returncode}; output={captured!r}"
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


def run_desk_layout_split_and_delete(desk, cube, env):
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
    started_extra = False
    opened_split_menu = False
    selected_extra = False
    saw_extra_pane = False
    sent_delete = False
    saw_deleted = False
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

            if not started_extra and b"layout-left" in captured:
                run_checked(
                    [
                        cube,
                        "run",
                        "--bg",
                        "--tty",
                        "--name",
                        "layout-extra",
                        sys.executable,
                        "-c",
                        (
                            "import sys,time,tty\n"
                            "tty.setraw(0)\n"
                            "sys.stdout.write('READY layout-extra\\n')\n"
                            "sys.stdout.flush()\n"
                            "time.sleep(10)\n"
                        ),
                    ],
                    env,
                )
                os.write(master_fd, b"\x18sH")
                opened_split_menu = True
                started_extra = True

            if opened_split_menu and not selected_extra:
                if b"select a cube for the new pane" in captured and b"layout-extra" in captured:
                    os.write(master_fd, b"\r")
                    selected_extra = True

            if selected_extra and not saw_extra_pane and b"layout-extra" in captured:
                saw_extra_pane = True
                captured.clear()
                os.write(master_fd, b"D")
                sent_delete = True

            if sent_delete and not saw_deleted:
                time.sleep(0.2)
                if b"layout-left" in captured and b"layout-extra" not in captured:
                    saw_deleted = True

            if saw_deleted and not sent_quit:
                os.write(master_fd, b"\x18q")
                sent_quit = True

            if proc.poll() is not None:
                break

        if not started_extra:
            raise AssertionError(f"desk did not render layout-left pane: {captured!r}")
        if not opened_split_menu:
            raise AssertionError("desk was not asked to open split menu")
        if not selected_extra:
            raise AssertionError(f"desk split menu did not expose layout-extra: {captured!r}")
        if not saw_extra_pane:
            raise AssertionError(f"desk did not attach split pane: {captured!r}")
        if not sent_delete:
            raise AssertionError("desk was not asked to delete split pane")
        if not saw_deleted:
            raise AssertionError(f"desk did not delete active split pane: {captured!r}")
        if proc.poll() is None:
            proc.wait(timeout=2)
        if proc.returncode != 0:
            raise AssertionError(
                f"desk split/delete exited with {proc.returncode}; output={captured!r}"
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


def run_desk_configurable_keys(desk, cube, env):
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
    sent_unbound = False
    saw_unbound = False
    sent_direct_quit = False
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

            if not sent_unbound and b"desk-safe" in captured:
                os.write(master_fd, b"\x01m")
                sent_unbound = True

            if sent_unbound and not saw_unbound:
                logs = subprocess.run(
                    [cube, "logs", "--stdout", "desk-safe"],
                    env=env,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                if "GOT_PREFIX_M" in logs.stdout:
                    saw_unbound = True

            if saw_unbound and not sent_direct_quit:
                os.write(master_fd, b"\x1b[1;6C")
                sent_direct_quit = True

            if proc.poll() is not None:
                break

        if not sent_unbound:
            raise AssertionError(f"desk did not render configurable-key pane: {captured!r}")
        if not saw_unbound:
            raise AssertionError(f"unbound Prefix-m was not forwarded: {captured!r}")
        if not sent_direct_quit:
            raise AssertionError("modified direct quit shortcut was not sent")
        if proc.poll() is None:
            proc.wait(timeout=2)
        if proc.returncode != 0:
            raise AssertionError(
                f"desk direct quit exited with {proc.returncode}; output={captured!r}"
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


def run_desk_bindings_overlay(desk, env):
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
    sent_overlay = False
    saw_overlay = False
    sent_edit = False
    saw_edit_prompt = False
    sent_new_key = False
    saw_edited_binding = False
    sent_close = False
    sent_reopen = False
    saw_reopened_overlay = False
    overlay_count_before_reopen = 0
    closed_at = 0.0
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

            if not sent_overlay and b"desk-safe" in captured:
                os.write(master_fd, b"\x18?")
                sent_overlay = True

            if sent_overlay and not saw_overlay:
                if (
                    b"Key bindings" in captured
                    and b"[bindings.show]" in captured
                    and b"Prefix-?" in captured
                    and b"[layout.save]" in captured
                    and b"Prefix-:" in captured
                ):
                    saw_overlay = True

            if saw_overlay and not sent_edit:
                os.write(master_fd, b"j" * 17 + b"e")
                sent_edit = True

            if sent_edit and not saw_edit_prompt:
                if b"Edit binding" in captured and b"Key:" in captured:
                    saw_edit_prompt = True

            if saw_edit_prompt and not sent_new_key:
                os.write(master_fd, b"\x15Control-G\r")
                sent_new_key = True

            if sent_new_key and not saw_edited_binding:
                if (
                    b"Key bindings" in captured
                    and b"[bindings.show]" in captured
                    and b"C-G" in captured
                ):
                    saw_edited_binding = True

            if saw_edited_binding and not sent_close:
                overlay_count_before_reopen = captured.count(b"Key bindings")
                os.write(master_fd, b"q")
                sent_close = True
                closed_at = time.time()

            if sent_close and not sent_reopen and time.time() - closed_at > 0.1:
                os.write(master_fd, b"\x07")
                sent_reopen = True

            if sent_reopen and not saw_reopened_overlay:
                key_bindings_count = captured.count(b"Key bindings")
                if key_bindings_count > overlay_count_before_reopen:
                    saw_reopened_overlay = True
                    os.write(master_fd, b"q")

            if proc.poll() is not None:
                break

        if not sent_overlay:
            raise AssertionError(f"desk did not render bindings pane: {captured!r}")
        if not saw_overlay:
            raise AssertionError(f"desk did not show bindings overlay: {captured!r}")
        if not sent_edit:
            raise AssertionError("desk binding edit was not requested")
        if not saw_edit_prompt:
            raise AssertionError(f"desk did not show binding edit prompt: {captured!r}")
        if not sent_new_key:
            raise AssertionError("desk edited binding was not submitted")
        if not saw_edited_binding:
            raise AssertionError(f"desk did not show edited binding: {captured!r}")
        if not sent_close:
            raise AssertionError("desk was not asked to close bindings overlay")
        if not sent_reopen:
            raise AssertionError("desk edited binding was not exercised")
        if not saw_reopened_overlay:
            raise AssertionError(f"desk edited binding did not reopen overlay: {captured!r}")
        if proc.poll() is None:
            proc.terminate()
            proc.wait(timeout=2)
        if proc.returncode not in (0, -signal.SIGTERM):
            raise AssertionError(
                f"desk bindings overlay exited with {proc.returncode}; output={captured!r}"
            )
        config_path = os.path.join(
            env["XDG_CONFIG_HOME"], "cubicle", "config.cfg"
        )
        with open(config_path, "r", encoding="utf-8") as handle:
            config_text = handle.read()
        if "cubicle-desk-managed-keys: begin" not in config_text:
            raise AssertionError(f"desk did not persist managed bindings: {config_text}")
        if "C-G bindings.show" not in config_text:
            raise AssertionError(f"desk did not persist edited binding: {config_text}")
        if "Prefix-? none" not in config_text:
            raise AssertionError(f"desk did not persist old binding unbind: {config_text}")
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        os.close(master_fd)


def run_desk_click_inactive_title(desk, cube, env):
    master_fd, slave_fd = pty.openpty()
    proc = subprocess.Popen(
        [desk, "--mouse"],
        stdin=slave_fd,
        stdout=slave_fd,
        stderr=subprocess.PIPE,
        env=env,
        close_fds=True,
    )
    os.close(slave_fd)
    captured = bytearray()
    clicked_title = False
    saw_active_title = False
    title_row = 0
    title_col = 0
    sent_payload = False
    saw_payload = False
    sent_ctrl_select = False
    saw_mouse_release = False
    mouse_release_start = 0
    sent_mouse_toggle = False
    saw_mouse_toggle_off = False
    sent_quit = False
    deadline = time.time() + 7
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

            if (
                not clicked_title
                and b"title-left" in captured
                and b"[title-right]" in captured
                and b"\x1b[?1000h\x1b[?1006h" in captured
            ):
                marker = captured.rfind(b"title-right")
                positions = re.findall(rb"\x1b\[(\d+);(\d+)H", captured[:marker])
                if positions:
                    title_row = int(positions[-1][0])
                    title_col = int(positions[-1][1])
                    click_col = title_col + 2
                    os.write(
                        master_fd,
                        f"\x1b[<0;{click_col};{title_row}M".encode("ascii"),
                    )
                    clicked_title = True

            if clicked_title and not saw_active_title:
                active_marker = (
                    f"\x1b[{title_row};{title_col}H\x1b[1;7m title-right"
                ).encode("ascii")
                if active_marker in captured:
                    saw_active_title = True

            if saw_active_title and not sent_payload:
                os.write(master_fd, b"TITLE")
                sent_payload = True

            if sent_payload and not saw_payload:
                logs = subprocess.run(
                    [cube, "logs", "--stdout", "title-right"],
                    env=env,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                if "GOT_TITLE" in logs.stdout:
                    saw_payload = True

            if saw_payload and not sent_ctrl_select:
                mouse_release_start = len(captured)
                os.write(master_fd, b"\x1b[<16;45;4M")
                sent_ctrl_select = True

            if sent_ctrl_select and not saw_mouse_release:
                release = b"\x1b[?1006l\x1b[?1000l"
                if release in captured[mouse_release_start:]:
                    saw_mouse_release = True

            if saw_mouse_release and not sent_mouse_toggle:
                captured.clear()
                os.write(master_fd, b"\x18m")
                sent_mouse_toggle = True

            if sent_mouse_toggle and not saw_mouse_toggle_off:
                if b"\x1b[1;1H\x1b[2m title-left " in captured:
                    if b"[title-left]" in captured:
                        raise AssertionError(
                            f"desk left inactive title bracketed after mouse toggle: {captured!r}"
                        )
                    saw_mouse_toggle_off = True

            if saw_mouse_toggle_off and not sent_quit:
                os.write(master_fd, b"\x18q")
                sent_quit = True

            if proc.poll() is not None:
                break

        if not clicked_title:
            raise AssertionError(f"desk did not render inactive title: {captured!r}")
        if not saw_active_title:
            raise AssertionError(f"desk did not select clicked title: {captured!r}")
        if not saw_payload:
            raise AssertionError(f"clicked title pane did not receive input: {captured!r}")
        if not saw_mouse_release:
            raise AssertionError(f"desk did not release mouse for Ctrl-selection: {captured!r}")
        if not saw_mouse_toggle_off:
            raise AssertionError(f"desk did not toggle mouse title mode off: {captured!r}")
        if proc.poll() is None:
            proc.wait(timeout=2)
        if proc.returncode != 0:
            raise AssertionError(
                f"desk exited with {proc.returncode}; output={captured!r}"
            )
    finally:
        os.close(master_fd)


def run_desk_open_other_workspace_process(desk, cube, env):
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
    opened_menu = False
    selected_workspace = False
    checked_workspace_scope = False
    selected_process = False
    sent_payload = False
    saw_payload = False
    checked_safe_macros = False
    opened_safe_macro_menu = False
    safe_macros_closed_at = 0.0
    reopened_menu = False
    saw_last_workspace_highlight = False
    checked_other_macros = False
    opened_other_macro_menu = False
    other_macros_closed_at = 0.0
    sent_macro = False
    saw_macro = False
    saw_macro_at = 0.0
    focused_return_pane = False
    checked_return_macros = False
    requested_return_macro_menu = False
    opened_return_macro_menu = False
    return_macros_closed_at = 0.0
    sent_return_macro = False
    saw_return_macro = False
    saw_return_macro_at = 0.0
    sent_quit = False
    deadline = time.time() + 10
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

            if not checked_safe_macros and b"desk-safe" in captured:
                captured.clear()
                os.write(master_fd, b"\x18o")
                checked_safe_macros = True

            if (
                checked_safe_macros
                and not opened_safe_macro_menu
                and not opened_menu
                and b"Open cube" in captured
            ):
                os.write(master_fd, b"m")
                opened_safe_macro_menu = True

            if opened_safe_macro_menu and not opened_menu and b"Macros" in captured:
                if b"1. safe-only" not in captured:
                    continue
                if b"other-only" in captured:
                    raise AssertionError(
                        f"safe workspace macro menu leaked other macro: {captured!r}"
                    )
                os.write(master_fd, b"q")
                captured.clear()
                safe_macros_closed_at = time.time()

            if (
                safe_macros_closed_at > 0.0
                and not opened_menu
                and time.time() - safe_macros_closed_at > 0.2
            ):
                os.write(master_fd, b"\x18o")
                opened_menu = True

            if opened_menu and not selected_workspace and b"workspace: DeskOther" in captured:
                os.write(master_fd, b"j\x1b[C")
                selected_workspace = True
                captured.clear()

            if (
                selected_workspace
                and not checked_workspace_scope
                and b"Workspace: DeskOther" in captured
                and b"desk-other" in captured
            ):
                submenu = captured[captured.rfind(b"Workspace: DeskOther"):]
                leaked_current_item = re.search(
                    rb"\x1b\[[0-9]+;8H(?:\x1b\[[0-9;]*m)*desk-safe",
                    submenu,
                )
                if leaked_current_item is not None:
                    raise AssertionError(
                        f"workspace submenu included current workspace cube: {captured!r}"
                    )
                checked_workspace_scope = True

            if checked_workspace_scope and not selected_process and b"desk-other" in captured:
                os.write(master_fd, b"\r")
                selected_process = True

            if selected_process and not sent_payload and b"READY desk-other" in captured:
                os.write(master_fd, b"OPEN")
                sent_payload = True

            if sent_payload and not saw_payload:
                logs = subprocess.run(
                    [cube, "--workspace", "DeskOther", "logs", "--stdout", "desk-other"],
                    env=env,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                if "GOT_OPEN" in logs.stdout:
                    saw_payload = True

            if saw_payload and not reopened_menu:
                captured.clear()
                os.write(master_fd, b"\x18o")
                reopened_menu = True

            if (
                reopened_menu
                and not opened_other_macro_menu
                and not checked_other_macros
                and b"Open cube" in captured
            ):
                os.write(master_fd, b"m")
                opened_other_macro_menu = True

            if opened_other_macro_menu and not checked_other_macros and b"Macros" in captured:
                if b"1. other-only" not in captured:
                    continue
                if b"safe-only" in captured:
                    raise AssertionError(
                        f"other workspace macro menu leaked safe macro: {captured!r}"
                    )
                checked_other_macros = True
                os.write(master_fd, b"q")
                captured.clear()
                other_macros_closed_at = time.time()

            if (
                other_macros_closed_at > 0.0
                and not sent_macro
                and time.time() - other_macros_closed_at > 0.2
            ):
                os.write(master_fd, b"\x181")
                sent_macro = True

            if sent_macro and not saw_macro:
                logs = subprocess.run(
                    [cube, "--workspace", "DeskOther", "logs", "--stdout", "desk-other"],
                    env=env,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                if "GOT_OTHER_MACRO" in logs.stdout:
                    saw_macro = True
                    saw_macro_at = time.time()
                    captured.clear()

            if reopened_menu and not saw_last_workspace_highlight:
                if re.search(
                    rb"\x1b\[[0-9]+;[0-9]+H\x1b\[1;7;48;5;236mworkspace: DeskOther",
                    captured,
                ):
                    saw_last_workspace_highlight = True

            if (
                saw_macro
                and saw_last_workspace_highlight
                and not focused_return_pane
                and time.time() - saw_macro_at > 0.2
            ):
                os.write(master_fd, b"\x18n")
                focused_return_pane = True
                captured.clear()

            if (
                focused_return_pane
                and not opened_return_macro_menu
                and not checked_return_macros
                and b"desk-safe-macro" in captured
            ):
                captured.clear()
                os.write(master_fd, b"\x18o")
                opened_return_macro_menu = True

            if (
                opened_return_macro_menu
                and not requested_return_macro_menu
                and not checked_return_macros
                and b"Open cube" in captured
            ):
                os.write(master_fd, b"m")
                requested_return_macro_menu = True
                captured.clear()

            if opened_return_macro_menu and not checked_return_macros and b"Macros" in captured:
                if b"1. safe-only" not in captured:
                    continue
                if b"other-only" in captured:
                    raise AssertionError(
                        f"returned safe workspace macro menu leaked other macro: {captured!r}"
                    )
                checked_return_macros = True
                os.write(master_fd, b"q")
                captured.clear()
                return_macros_closed_at = time.time()

            if (
                return_macros_closed_at > 0.0
                and not sent_return_macro
                and time.time() - return_macros_closed_at > 0.2
            ):
                os.write(master_fd, b"\x181")
                sent_return_macro = True

            if sent_return_macro and not saw_return_macro:
                logs = subprocess.run(
                    [
                        cube,
                        "--workspace",
                        "DeskSafe",
                        "logs",
                        "--stdout",
                        "desk-safe-macro",
                    ],
                    env=env,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                if "GOT_SAFE_MACRO" in logs.stdout:
                    saw_return_macro = True
                    saw_return_macro_at = time.time()
                    captured.clear()

            if saw_return_macro and not sent_quit:
                if time.time() - saw_return_macro_at > 0.2:
                    os.write(master_fd, b"\x18q")
                    sent_quit = True

            if proc.poll() is not None:
                break

        if not opened_menu:
            raise AssertionError(f"desk did not render initial pane: {captured!r}")
        if not checked_safe_macros:
            raise AssertionError(f"desk did not show safe workspace macros: {captured!r}")
        if not selected_workspace:
            raise AssertionError(f"desk open menu did not show other workspace: {captured!r}")
        if not checked_workspace_scope:
            raise AssertionError(f"desk did not render scoped workspace submenu: {captured!r}")
        if not selected_process:
            raise AssertionError(f"desk open menu did not show other process: {captured!r}")
        if not saw_payload:
            raise AssertionError(f"newly opened pane did not receive input: {captured!r}")
        if not checked_other_macros:
            raise AssertionError(f"desk did not show other workspace macros: {captured!r}")
        if not sent_macro:
            raise AssertionError("desk did not run the other workspace macro")
        if not saw_macro:
            raise AssertionError(f"other workspace macro did not reach target: {captured!r}")
        if not saw_last_workspace_highlight:
            raise AssertionError(
                f"desk did not highlight the last opened workspace: {captured!r}"
            )
        if not focused_return_pane:
            raise AssertionError("desk was not asked to refocus the safe workspace pane")
        if not checked_return_macros:
            raise AssertionError(
                f"desk did not restore safe workspace macros: {captured!r}"
            )
        if not sent_return_macro:
            raise AssertionError("desk did not run the returned safe workspace macro")
        if not saw_return_macro:
            raise AssertionError(
                f"returned safe workspace macro did not reach target: {captured!r}"
            )
        if not sent_quit:
            raise AssertionError("desk was not asked to quit")
        if proc.poll() is None:
            proc.wait(timeout=2)
        if proc.returncode != 0:
            raise AssertionError(
                f"desk exited with {proc.returncode}; output={captured!r}"
            )
    finally:
        os.close(master_fd)


def run_desk_new_process_from_menu(desk, cube, env, command, expected_name):
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
    opened_menu = False
    selected_new = False
    entered_command = False
    saw_new_process = False
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

            if not opened_menu and b"new-left" in captured:
                os.write(master_fd, b"\x18o")
                opened_menu = True

            if opened_menu and not selected_new and b"New" in captured:
                os.write(master_fd, b"k\r")
                selected_new = True

            if selected_new and not entered_command and b"Command:" in captured:
                os.write(master_fd, command.encode("utf-8") + b"\r")
                entered_command = True

            if entered_command and not saw_new_process and b"READY_DESK_NEW" in captured:
                ps_output = run_checked([cube, "ps"], env)
                if f"{expected_name}\ttty\trunning" not in ps_output:
                    raise AssertionError(
                        f"new desk process was not running:\n{ps_output}"
                    )
                saw_new_process = True

            if saw_new_process and not sent_quit:
                os.write(master_fd, b"\x18q")
                sent_quit = True

            if proc.poll() is not None:
                break

        if not opened_menu:
            raise AssertionError(f"desk did not render initial pane: {captured!r}")
        if not selected_new:
            raise AssertionError(f"desk open menu did not expose New: {captured!r}")
        if not entered_command:
            raise AssertionError(f"desk did not render command prompt: {captured!r}")
        if not saw_new_process:
            raise AssertionError(f"new desk process did not render: {captured!r}")
        if proc.poll() is None:
            proc.wait(timeout=2)
        if proc.returncode != 0:
            raise AssertionError(
                f"desk exited with {proc.returncode}; output={captured!r}"
            )
    finally:
        os.close(master_fd)


def run_desk_new_process_from_workspace_menu(
    desk,
    cube,
    env,
    workspace,
    command,
    expected_name,
    open_from_empty_pane=False,
    workspace_steps_before_open=0,
    empty_pane_marker=b"[4]",
    empty_pane_next_count=3,
):
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
    opened_menu = False
    selected_empty_pane = False
    opened_workspace = False
    drove_hidden_workspace = False
    selected_new = False
    entered_command = False
    saw_new_process = False
    sent_quit = False
    workspace_marker = f"workspace: {workspace}".encode("utf-8")
    submenu_marker = f"Workspace: {workspace}".encode("utf-8")
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

            if (
                open_from_empty_pane
                and not selected_empty_pane
                and b"empty-left" in captured
                and b"empty-middle" in captured
                and b"empty-right" in captured
            ):
                os.write(master_fd, b"\x18n" * empty_pane_next_count)
                selected_empty_pane = True

            if not opened_menu and (
                (not open_from_empty_pane and b"desk-safe" in captured)
                or (
                    open_from_empty_pane
                    and selected_empty_pane
                    and empty_pane_marker in captured
                )
            ):
                os.write(master_fd, b"\x18o")
                opened_menu = True

            if opened_menu and not opened_workspace and workspace_marker in captured:
                os.write(master_fd, b"j" * workspace_steps_before_open + b"\x1b[C")
                opened_workspace = True
                captured.clear()

            if (
                open_from_empty_pane
                and opened_menu
                and not opened_workspace
                and not drove_hidden_workspace
                and b"Open cube" in captured
            ):
                os.write(master_fd, b"j" * workspace_steps_before_open + b"\x1b[C")
                opened_workspace = True
                drove_hidden_workspace = True
                captured.clear()

            if (
                opened_workspace
                and not selected_new
                and (submenu_marker in captured or open_from_empty_pane)
                and (
                    b"workspace has no running cubes" in captured
                    or (
                        open_from_empty_pane
                        and b"workspace has no" in captured
                    )
                )
                and b"New" in captured
            ):
                os.write(master_fd, b"\r")
                selected_new = True

            if selected_new and not entered_command and b"Command:" in captured:
                os.write(master_fd, command.encode("utf-8") + b"\r")
                entered_command = True

            if entered_command and not saw_new_process and b"READY_WORKSPACE_NEW" in captured:
                ps_output = run_checked([cube, "--workspace", workspace, "ps"], env)
                if f"{expected_name}\ttty\trunning" not in ps_output:
                    raise AssertionError(
                        f"new workspace menu process was not running:\n{ps_output}"
                    )
                saw_new_process = True

            if saw_new_process and not sent_quit:
                os.write(master_fd, b"\x18q")
                sent_quit = True

            if proc.poll() is not None:
                break

        if not opened_menu:
            raise AssertionError(f"desk did not render initial pane: {captured!r}")
        if open_from_empty_pane and not selected_empty_pane:
            raise AssertionError(f"desk did not select empty pane: {captured!r}")
        if not opened_workspace:
            raise AssertionError(
                f"desk open menu did not expose workspace {workspace}: {captured!r}"
            )
        if not selected_new:
            raise AssertionError(
                f"workspace menu did not expose New with no-running message: {captured!r}"
            )
        if not entered_command:
            raise AssertionError(f"desk did not render command prompt: {captured!r}")
        if not saw_new_process:
            raise AssertionError(
                f"new workspace menu process did not render: {captured!r}"
            )
        if proc.poll() is None:
            proc.wait(timeout=2)
        if proc.returncode != 0:
            raise AssertionError(
                f"desk exited with {proc.returncode}; output={captured!r}"
            )
    finally:
        os.close(master_fd)


def run_desk_enter_workspace_switch(desk, cube, env):
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
    opened_menu = False
    selected_workspace = False
    sent_payload = False
    saw_payload = False
    sent_quit = False
    deadline = time.time() + 7
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

            if not opened_menu and b"desk-safe" in captured:
                os.write(master_fd, b"\x18o")
                opened_menu = True

            if opened_menu and not selected_workspace and b"workspace: DeskEnter" in captured:
                os.write(master_fd, b"\r")
                selected_workspace = True

            if selected_workspace and not sent_payload and b"READY desk-enter" in captured:
                os.write(master_fd, b"ENTER")
                sent_payload = True

            if sent_payload and not saw_payload:
                logs = subprocess.run(
                    [cube, "--workspace", "DeskEnter", "logs", "--stdout", "desk-enter"],
                    env=env,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                if "GOT_ENTER" in logs.stdout:
                    saw_payload = True

            if saw_payload and not sent_quit:
                os.write(master_fd, b"\x18q")
                sent_quit = True

            if proc.poll() is not None:
                break

        if not opened_menu:
            raise AssertionError(f"desk did not render initial pane: {captured!r}")
        if not selected_workspace:
            raise AssertionError(f"desk open menu did not show enter workspace: {captured!r}")
        if not saw_payload:
            raise AssertionError(f"entered workspace pane did not receive input: {captured!r}")
        with open(desk_default_workspace_file(env), "r", encoding="utf-8") as handle:
            if handle.read().strip() != "DeskEnter":
                raise AssertionError("desk did not persist entered workspace")
        if os.path.exists(desk_default_layout_file(env)):
            raise AssertionError("desk workspace switch should clear default layout")
        if proc.poll() is None:
            proc.wait(timeout=2)
        if proc.returncode != 0:
            raise AssertionError(
                f"desk exited with {proc.returncode}; output={captured!r}"
            )
        clear_desk_defaults(env)
    finally:
        os.close(master_fd)


def run_desk_click_workspace_switch(desk, cube, env):
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
    opened_menu = False
    clicked_workspace = False
    sent_payload = False
    saw_payload = False
    sent_quit = False
    deadline = time.time() + 7
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

            if not opened_menu and b"desk-safe" in captured:
                os.write(master_fd, b"\x18o")
                opened_menu = True

            if (
                opened_menu
                and not clicked_workspace
                and b"workspace: DeskClick" in captured
                and b"\x1b[?1000h\x1b[?1006h" in captured
            ):
                marker = captured.rfind(b"workspace: DeskClick")
                positions = re.findall(rb"\x1b\[(\d+);(\d+)H", captured[:marker])
                if positions:
                    row = int(positions[-1][0])
                    col = int(positions[-1][1]) + 2
                    os.write(master_fd, f"\x1b[<0;{col};{row}M".encode("ascii"))
                    clicked_workspace = True

            if clicked_workspace and not sent_payload and b"READY desk-click" in captured:
                os.write(master_fd, b"CLICK")
                sent_payload = True

            if sent_payload and not saw_payload:
                logs = subprocess.run(
                    [cube, "--workspace", "DeskClick", "logs", "--stdout", "desk-click"],
                    env=env,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                if "GOT_CLICK" in logs.stdout:
                    saw_payload = True

            if saw_payload and not sent_quit:
                os.write(master_fd, b"\x18q")
                sent_quit = True

            if proc.poll() is not None:
                break

        if not opened_menu:
            raise AssertionError(f"desk did not render initial pane: {captured!r}")
        if not clicked_workspace:
            raise AssertionError(f"desk open menu did not expose clickable workspace: {captured!r}")
        if not saw_payload:
            raise AssertionError(f"clicked workspace did not replace desk session: {captured!r}")
        with open(desk_default_workspace_file(env), "r", encoding="utf-8") as handle:
            if handle.read().strip() != "DeskClick":
                raise AssertionError("desk did not persist clicked workspace")
        if os.path.exists(desk_default_layout_file(env)):
            raise AssertionError("desk workspace switch should clear default layout")
        if proc.poll() is None:
            proc.wait(timeout=2)
        if proc.returncode != 0:
            raise AssertionError(
                f"desk exited with {proc.returncode}; output={captured!r}"
            )
        clear_desk_defaults(env)
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


def run_desk_scrollback_returns_to_live(desk, env):
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
    sent_generate = False
    sent_scroll = False
    saw_history = False
    sent_scroll_exit = False
    sent_input = False
    saw_live_restore = False
    sent_quit = False
    deadline = time.time() + 6
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

            if not sent_generate and b"READY_SCROLLBACK" in captured:
                os.write(master_fd, b"g")
                sent_generate = True

            if sent_generate and not sent_scroll and b"LIVE_MARKER" in captured:
                captured.clear()
                os.write(master_fd, b"\x18\x1b[5~")
                sent_scroll = True

            if sent_scroll and not saw_history and b"HISTORY_00" in captured:
                saw_history = True
                captured.clear()
                os.write(master_fd, b"q")
                sent_scroll_exit = True

            if sent_scroll_exit and not sent_input and b"scroll mode" not in captured:
                os.write(master_fd, b"x")
                sent_input = True

            if sent_input and not saw_live_restore and b"LIVE_MARKER" in captured:
                saw_live_restore = True
                os.write(master_fd, b"\x18q")
                sent_quit = True

            if proc.poll() is not None:
                break

        if not sent_generate:
            raise AssertionError(
                f"desk did not render scrollback generator pane: {captured!r}"
            )
        if not sent_scroll:
            raise AssertionError(
                f"desk did not render generated live screen: {captured!r}"
            )
        if not saw_history:
            raise AssertionError(
                f"desk did not scroll active pane into history: {captured!r}"
            )
        if not sent_scroll_exit:
            raise AssertionError("desk was not asked to exit scroll mode")
        if not sent_input:
            raise AssertionError("desk was not sent input to return to live")
        if not saw_live_restore:
            raise AssertionError(
                f"desk did not restore latest live screen after input: {captured!r}"
            )
        if not sent_quit:
            raise AssertionError("desk was not asked to quit")
        if proc.poll() is None:
            proc.wait(timeout=2)
        if proc.returncode != 0:
            raise AssertionError(
                f"desk scrollback restore exited with {proc.returncode}; output={captured!r}"
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
    cursor_draw = b"\x1b[3;15H\x1b[0m\x1b[7m"
    active_title = b"\x1b[1;7m cursor-"
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
        if active_title not in captured:
            raise AssertionError(
                f"desk did not draw highlighted pane title: {captured!r}"
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
        env["TERM"] = "xterm-256color"
        os.makedirs(env["XDG_STATE_HOME"], exist_ok=True)
        os.makedirs(env["XDG_RUNTIME_DIR"], exist_ok=True)
        os.makedirs(env["XDG_CONFIG_HOME"], exist_ok=True)
        os.makedirs(os.path.join(env["XDG_CONFIG_HOME"], "cubicle"), exist_ok=True)
        config_path = os.path.join(
            env["XDG_CONFIG_HOME"], "cubicle", "config.cfg"
        )
        write_test_config(config_path, log_dir)

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
                    "deadline=time.time()+16\n"
                    "data=b''\n"
                    "saw_ctrl_c=False\n"
                    "saw_ping=False\n"
                    "saw_prefix_m=False\n"
                    "saw_layout=False\n"
                    "saw_leak_start=False\n"
                    "saw_layout_leak=False\n"
                    "saw_scroll_start=False\n"
                    "saw_scroll_done=False\n"
                    "saw_scroll_leak=False\n"
                    "while time.time()<deadline:\n"
                    "    r,_,_=select.select([sys.stdin],[],[],0.05)\n"
                    "    if r:\n"
                    "        data+=os.read(0,64)\n"
                    "    if not saw_ctrl_c and b'\\x03' in data:\n"
                    "        sys.stdout.write('GOT_CTRL_C\\n')\n"
                    "        sys.stdout.flush()\n"
                    "        saw_ctrl_c=True\n"
                    "    if not saw_ping and b'PING' in data:\n"
                    "        sys.stdout.write('GOT_PING\\n')\n"
                    "        sys.stdout.flush()\n"
                    "        saw_ping=True\n"
                    "    if not saw_prefix_m and b'\\x01m' in data:\n"
                    "        sys.stdout.write('GOT_PREFIX_M\\n')\n"
                    "        sys.stdout.flush()\n"
                    "        saw_prefix_m=True\n"
                    "    if not saw_leak_start and b'LEAK_START' in data:\n"
                    "        sys.stdout.write('GOT_LEAK_START\\n')\n"
                    "        sys.stdout.flush()\n"
                    "        saw_leak_start=True\n"
                    "    if not saw_layout and b'LAYOUT' in data:\n"
                    "        sys.stdout.write('GOT_LAYOUT\\n')\n"
                    "        sys.stdout.flush()\n"
                    "        saw_layout=True\n"
                    "    if saw_leak_start and not saw_layout and not saw_layout_leak:\n"
                    "        tail=data.split(b'LEAK_START', 1)[1]\n"
                    "        if any(ch in tail for ch in (b'h', b'v', b'r', b't')):\n"
                    "            sys.stdout.write('GOT_LAYOUT_LEAK '+tail.hex()+'\\n')\n"
                    "            sys.stdout.flush()\n"
                    "            saw_layout_leak=True\n"
                    "    if not saw_scroll_start and b'SCROLL_START' in data:\n"
                    "        sys.stdout.write('GOT_SCROLL_START\\n')\n"
                    "        sys.stdout.flush()\n"
                    "        saw_scroll_start=True\n"
                    "    if saw_scroll_start and not saw_scroll_done and not saw_scroll_leak:\n"
                    "        tail=data.split(b'SCROLL_START', 1)[1]\n"
                    "        for seq in (b'\\x1b[A', b'\\x1b[B', b'\\x1b[C', b'\\x1b[D', b'\\x1b[5~', b'\\x1b[6~', b'\\x1b[H', b'\\x1b[F'):\n"
                    "            if seq in tail:\n"
                    "                sys.stdout.write('GOT_SCROLL_LEAK '+tail.hex()+'\\n')\n"
                    "                sys.stdout.flush()\n"
                    "                saw_scroll_leak=True\n"
                    "                break\n"
                    "    if not saw_scroll_done and b'SCROLL_DONE' in data:\n"
                    "        sys.stdout.write('GOT_SCROLL_DONE\\n')\n"
                    "        sys.stdout.flush()\n"
                    "        saw_scroll_done=True\n"
                    "    if saw_ctrl_c and saw_ping and saw_prefix_m and saw_layout and saw_scroll_done:\n"
                    "        break\n"
                    "time.sleep(10)\n"
                ),
            ],
            env,
        )

        run_desk_and_ctrl_c(desk, cube, env, log_dir)
        run_desk_scroll_mode_keys(desk, cube, env)
        run_desk_layout_mode_keys(desk, cube, env)
        run_checked([cube, "workspace", "create", "DeskLayout"], env)
        run_checked(
            [
                cube,
                "--workspace",
                "DeskLayout",
                "run",
                "--bg",
                "--tty",
                "--name",
                "layout-left",
                sys.executable,
                "-c",
                (
                    "import sys,time,tty\n"
                    "tty.setraw(0)\n"
                    "sys.stdout.write('READY layout-left\\n')\n"
                    "sys.stdout.flush()\n"
                    "time.sleep(10)\n"
                ),
            ],
            env,
        )
        run_checked([cube, "workspace", "DeskLayout"], env)
        run_desk_layout_split_and_delete(desk, cube, env)
        run_checked([cube, "kill", "--all", "--cleanup"], env)
        run_checked([cube, "workspace", "DeskSafe"], env)
        run_checked([cube, "workspace", "delete", "DeskLayout"], env)

        write_test_config(
            config_path,
            log_dir,
            (
                "[desk.keys]\n"
                "bind.11=Prefix-Left pane.left\n"
                "bind.12=Prefix-Right pane.right\n"
                "bind.13=Prefix-Up pane.above\n"
                "bind.14=Prefix-Down pane.below\n"
            ),
        )
        run_checked([cube, "workspace", "create", "DeskThree"], env)
        for name in ("three-top", "three-bottom", "three-right"):
            run_checked(
                [
                    cube,
                    "--workspace",
                    "DeskThree",
                    "run",
                    "--bg",
                    "--tty",
                    "--name",
                    name,
                    sys.executable,
                    "-c",
                    (
                        "import os,select,sys,time,tty\n"
                        "tty.setraw(0)\n"
                        f"sys.stdout.write('READY {name}\\n')\n"
                        "sys.stdout.flush()\n"
                        "deadline=time.time()+10\n"
                        "data=b''\n"
                        "markers=(b'HIT_BOTTOM',b'HIT_TOP',b'HIT_RIGHT',b'HIT_LEFT')\n"
                        "while time.time()<deadline:\n"
                        "    r,_,_=select.select([sys.stdin],[],[],0.05)\n"
                        "    if r:\n"
                        "        data+=os.read(0,64)\n"
                        "    for marker in markers:\n"
                        "        if marker in data:\n"
                        f"            sys.stdout.write('GOT {name} '+marker.decode()+'\\n')\n"
                        "            sys.stdout.flush()\n"
                        "            data=data.replace(marker,b'',1)\n"
                    ),
                ],
                env,
            )
        run_checked([cube, "workspace", "DeskThree"], env)
        run_desk_three_pane_default_layout(desk, cube, env)
        run_checked([cube, "kill", "--all", "--cleanup"], env)
        run_checked([cube, "workspace", "DeskSafe"], env)
        run_checked([cube, "workspace", "delete", "DeskThree"], env)
        write_test_config(config_path, log_dir)
        write_test_config(
            config_path,
            log_dir,
            (
                "prefix=Control-A\n"
                "\n"
                "[desk.keys]\n"
                "bind.1=Prefix-m none\n"
                "bind.2=C-S-Right quit\n"
            ),
        )
        run_desk_configurable_keys(desk, cube, env)
        write_test_config(config_path, log_dir)
        run_desk_bindings_overlay(desk, env)
        run_desk_save_and_load_layout(desk, env)

        run_checked(
            [
                cube,
                "run",
                "--bg",
                "--tty",
                "--name",
                "desk-dies",
                sys.executable,
                "-c",
                (
                    "import sys,time\n"
                    "sys.stdout.write('READY_DIES\\n')\n"
                    "sys.stdout.flush()\n"
                    "time.sleep(1.0)\n"
                ),
            ],
            env,
        )
        run_desk_survives_dead_pane(desk, cube, env)
        run_checked([cube, "cleanup"], env)

        run_checked([cube, "workspace", "create", "DeskWorkspaceNew"], env)
        run_checked([cube, "workspace", "DeskSafe"], env)
        run_desk_new_process_from_workspace_menu(
            desk,
            cube,
            env,
            "DeskWorkspaceNew",
            (
                f"{sys.executable} -c \"import sys,time;"
                "print('READY_WORKSPACE_NEW', flush=True);time.sleep(10)\""
            ),
            os.path.basename(sys.executable),
        )
        run_checked(
            [cube, "--workspace", "DeskWorkspaceNew", "kill", "--all", "--cleanup"],
            env,
        )
        run_checked([cube, "workspace", "DeskSafe"], env)
        run_checked([cube, "workspace", "delete", "DeskWorkspaceNew"], env)

        run_checked([cube, "workspace", "create", "DeskEmptyPane"], env)
        for name in (
            "empty-left",
            "empty-middle",
            "empty-right",
            "empty-four",
            "empty-five",
        ):
            run_checked(
                [
                    cube,
                    "--workspace",
                    "DeskEmptyPane",
                    "run",
                    "--bg",
                    "--tty",
                    "--name",
                    name,
                    sys.executable,
                    "-c",
                    (
                        "import sys,time,tty\n"
                        "tty.setraw(0)\n"
                        f"sys.stdout.write('READY {name}\\n')\n"
                        "sys.stdout.flush()\n"
                        "time.sleep(10)\n"
                    ),
                ],
                env,
            )
        run_checked([cube, "workspace", "create", "DeskEmptyTarget"], env)
        run_checked([cube, "workspace", "DeskEmptyPane"], env)
        run_desk_new_process_from_workspace_menu(
            desk,
            cube,
            env,
            "DeskEmptyTarget",
            "bash -lc 'echo READY_WORKSPACE_NEW; sleep 10'",
            "bash",
            open_from_empty_pane=True,
            workspace_steps_before_open=1,
            empty_pane_marker=b"[6]",
            empty_pane_next_count=5,
        )
        run_checked(
            [cube, "--workspace", "DeskEmptyTarget", "kill", "--all", "--cleanup"],
            env,
        )
        run_checked(
            [cube, "--workspace", "DeskEmptyPane", "kill", "--all", "--cleanup"],
            env,
        )
        run_checked([cube, "workspace", "DeskSafe"], env)
        run_checked([cube, "workspace", "delete", "DeskEmptyTarget"], env)
        run_checked([cube, "workspace", "delete", "DeskEmptyPane"], env)

        run_checked([cube, "workspace", "create", "DeskEnter"], env)
        run_checked(
            [
                cube,
                "--workspace",
                "DeskEnter",
                "run",
                "--bg",
                "--tty",
                "--name",
                "desk-enter",
                sys.executable,
                "-c",
                (
                    "import os,select,sys,time,tty\n"
                    "tty.setraw(0)\n"
                    "sys.stdout.write('READY desk-enter\\n')\n"
                    "sys.stdout.flush()\n"
                    "deadline=time.time()+10\n"
                    "data=b''\n"
                    "while time.time()<deadline:\n"
                    "    r,_,_=select.select([sys.stdin],[],[],0.05)\n"
                    "    if r:\n"
                    "        data+=os.read(0,64)\n"
                    "        if b'ENTER' in data:\n"
                    "            sys.stdout.write('GOT_ENTER\\n')\n"
                    "            sys.stdout.flush()\n"
                    "            break\n"
                    "time.sleep(10)\n"
                ),
            ],
            env,
        )
        run_checked([cube, "workspace", "DeskSafe"], env)
        run_desk_enter_workspace_switch(desk, cube, env)
        run_checked([cube, "--workspace", "DeskEnter", "kill", "--all", "--cleanup"], env)

        run_checked([cube, "workspace", "create", "DeskOther"], env)
        safe_workspace_id = find_workspace_id(cube, env, "DeskSafe")
        other_workspace_id = find_workspace_id(cube, env, "DeskOther")
        seed_workspace_macro(
            state_dir,
            safe_workspace_id,
            1,
            "safe-only",
            "SAFE_MACRO",
            "desk-safe-macro",
        )
        seed_workspace_macro(
            state_dir,
            other_workspace_id,
            1,
            "other-only",
            "OTHER_MACRO",
            "desk-other",
        )
        run_checked(
            [
                cube,
                "--workspace",
                "DeskOther",
                "run",
                "--bg",
                "--tty",
                "--name",
                "desk-other",
                sys.executable,
                "-c",
                (
                    "import os,select,sys,time,tty\n"
                    "tty.setraw(0)\n"
                    "sys.stdout.write('READY desk-other\\n')\n"
                    "sys.stdout.flush()\n"
                    "deadline=time.time()+10\n"
                    "data=b''\n"
                    "saw_open=False\n"
                    "saw_macro=False\n"
                    "while time.time()<deadline:\n"
                    "    r,_,_=select.select([sys.stdin],[],[],0.05)\n"
                    "    if r:\n"
                    "        data+=os.read(0,64)\n"
                    "        if not saw_open and b'OPEN' in data:\n"
                    "            sys.stdout.write('GOT_OPEN\\n')\n"
                    "            sys.stdout.flush()\n"
                    "            saw_open=True\n"
                    "        if not saw_macro and b'OTHER_MACRO' in data:\n"
                    "            sys.stdout.write('GOT_OTHER_MACRO\\n')\n"
                    "            sys.stdout.flush()\n"
                    "            saw_macro=True\n"
                    "        if saw_open and saw_macro:\n"
                    "            break\n"
                    "time.sleep(10)\n"
                ),
            ],
            env,
        )
        run_checked(
            [
                cube,
                "--workspace",
                "DeskSafe",
                "run",
                "--bg",
                "--tty",
                "--name",
                "desk-safe-macro",
                sys.executable,
                "-c",
                (
                    "import os,select,sys,time,tty\n"
                    "tty.setraw(0)\n"
                    "sys.stdout.write('READY desk-safe-macro\\n')\n"
                    "sys.stdout.flush()\n"
                    "deadline=time.time()+12\n"
                    "data=b''\n"
                    "while time.time()<deadline:\n"
                    "    r,_,_=select.select([sys.stdin],[],[],0.05)\n"
                    "    if r:\n"
                    "        data+=os.read(0,64)\n"
                    "        if b'SAFE_MACRO' in data:\n"
                    "            sys.stdout.write('GOT_SAFE_MACRO\\n')\n"
                    "            sys.stdout.flush()\n"
                    "            break\n"
                    "time.sleep(10)\n"
                ),
            ],
            env,
        )
        run_checked([cube, "workspace", "DeskSafe"], env)
        run_desk_open_other_workspace_process(desk, cube, env)
        run_checked([cube, "--workspace", "DeskOther", "kill", "--all", "--cleanup"], env)

        library_log = os.path.join(log_dir, "client-library.log")
        if not os.path.exists(library_log):
            raise AssertionError("desk.debug=library did not create client library log")
        with open(library_log, "r", encoding="utf-8") as handle:
            library_events = handle.read()
        if "program=desk " not in library_events:
            raise AssertionError(f"desk library log missing program marker:\n{library_events}")
        if "event=rpc.request method=workspace.get code=ok" not in library_events:
            raise AssertionError(f"desk library log missing workspace RPC:\n{library_events}")
        if "event=attachment.stream_start method=tty code=ok" not in library_events:
            raise AssertionError(f"desk library log missing TTY stream start:\n{library_events}")
        if "event=attachment.stream_read method=tty code=ok" not in library_events:
            raise AssertionError(f"desk library log missing TTY stream read:\n{library_events}")
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

        run_checked([cube, "workspace", "create", "DeskNew"], env)
        run_checked(
            [
                cube,
                "--workspace",
                "DeskNew",
                "run",
                "--bg",
                "--tty",
                "--name",
                "new-left",
                sys.executable,
                "-c",
                (
                    "import sys,time,tty\n"
                    "tty.setraw(0)\n"
                    "sys.stdout.write('READY new-left\\n')\n"
                    "sys.stdout.flush()\n"
                    "time.sleep(10)\n"
                ),
            ],
            env,
        )
        run_checked([cube, "workspace", "DeskNew"], env)
        run_desk_new_process_from_menu(
            desk,
            cube,
            env,
            (
                f"{sys.executable} -c \"import sys,time;"
                "print('READY_DESK_NEW', flush=True);time.sleep(10)\""
            ),
            os.path.basename(sys.executable),
        )
        run_checked([cube, "kill", "--all", "--cleanup"], env)
        run_checked([cube, "workspace", "DeskSafe"], env)

        run_checked([cube, "workspace", "create", "DeskTitle"], env)
        run_checked(
            [
                cube,
                "--workspace",
                "DeskTitle",
                "run",
                "--bg",
                "--tty",
                "--name",
                "title-left",
                sys.executable,
                "-c",
                (
                    "import sys,time,tty\n"
                    "tty.setraw(0)\n"
                    "sys.stdout.write('READY title-left\\n')\n"
                    "sys.stdout.flush()\n"
                    "time.sleep(10)\n"
                ),
            ],
            env,
        )
        run_checked(
            [
                cube,
                "--workspace",
                "DeskTitle",
                "run",
                "--bg",
                "--tty",
                "--name",
                "title-right",
                sys.executable,
                "-c",
                (
                    "import os,select,sys,time,tty\n"
                    "tty.setraw(0)\n"
                    "sys.stdout.write('READY title-right\\n')\n"
                    "sys.stdout.flush()\n"
                    "deadline=time.time()+10\n"
                    "data=b''\n"
                    "while time.time()<deadline:\n"
                    "    r,_,_=select.select([sys.stdin],[],[],0.05)\n"
                    "    if r:\n"
                    "        data+=os.read(0,64)\n"
                    "        if b'TITLE' in data:\n"
                    "            sys.stdout.write('GOT_TITLE\\n')\n"
                    "            sys.stdout.flush()\n"
                    "            break\n"
                    "time.sleep(5)\n"
                ),
            ],
            env,
        )
        run_checked([cube, "workspace", "DeskTitle"], env)
        run_desk_click_inactive_title(desk, cube, env)
        run_checked([cube, "kill", "--all", "--cleanup"], env)

        run_checked([cube, "workspace", "create", "DeskClick"], env)
        run_checked(
            [
                cube,
                "--workspace",
                "DeskClick",
                "run",
                "--bg",
                "--tty",
                "--name",
                "desk-click",
                sys.executable,
                "-c",
                (
                    "import os,select,sys,time,tty\n"
                    "tty.setraw(0)\n"
                    "sys.stdout.write('READY desk-click\\n')\n"
                    "sys.stdout.flush()\n"
                    "deadline=time.time()+10\n"
                    "data=b''\n"
                    "while time.time()<deadline:\n"
                    "    r,_,_=select.select([sys.stdin],[],[],0.05)\n"
                    "    if r:\n"
                    "        data+=os.read(0,64)\n"
                    "        if b'CLICK' in data:\n"
                    "            sys.stdout.write('GOT_CLICK\\n')\n"
                    "            sys.stdout.flush()\n"
                    "            break\n"
                    "time.sleep(10)\n"
                ),
            ],
            env,
        )
        run_checked([cube, "workspace", "DeskSafe"], env)
        run_desk_click_workspace_switch(desk, cube, env)
        run_checked([cube, "--workspace", "DeskClick", "kill", "--all", "--cleanup"], env)

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
                "scrollback-live",
                sys.executable,
                "-c",
                (
                    "import os,select,sys,time,tty\n"
                    "tty.setraw(0)\n"
                    "sys.stdout.write('READY_SCROLLBACK\\r\\n')\n"
                    "sys.stdout.flush()\n"
                    "deadline=time.time()+8\n"
                    "generated=False\n"
                    "while time.time()<deadline:\n"
                    "    r,_,_=select.select([sys.stdin],[],[],0.05)\n"
                    "    if r:\n"
                    "        data=os.read(0,64)\n"
                    "        if b'g' in data and not generated:\n"
                    "            generated=True\n"
                    "            for i in range(40):\n"
                    "                sys.stdout.write('HISTORY_%02d\\r\\n' % i)\n"
                    "            sys.stdout.write('\\x1b[2J\\x1b[H LIVE_MARKER')\n"
                    "            sys.stdout.flush()\n"
                    "time.sleep(1)\n"
                ),
            ],
            env,
        )
        run_desk_scrollback_returns_to_live(desk, env)

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
