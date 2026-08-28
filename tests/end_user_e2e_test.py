#!/usr/bin/env python3
import shutil
import sys
import time

from end_user_harness import CubicleEndUserHarness, strip_terminal_sequences


def visible_text(pty_process):
    return strip_terminal_sequences(bytes(pty_process.output)).decode(
        "utf-8", errors="replace"
    )


def launch_less_cube(harness, name):
    long_file = harness.make_long_file(f"{name}.txt")
    harness.run_cube(
        [
            "run",
            "--bg",
            "--tty",
            "--name",
            name,
            "less",
            "-S",
            long_file,
        ]
    )


def assert_manager_responsive(harness):
    output = harness.run_cube(["ps"])
    if "Workspace EndUser" not in output:
        raise AssertionError(f"manager did not return current workspace ps:\n{output}")


def run_cube_connect_less_arrows(harness):
    launch_less_cube(harness, "less-cube")
    connected = harness.spawn_cube_pty(["connect", "less-cube"], rows=24, cols=80)
    try:
        connected.wait_for_text("LESS_LINE_0001")
        connected.output.clear()

        connected.send_user_arrow("down", "cube-connect")
        connected.wait_for_text("LESS_LINE_0024")
        connected.output.clear()

        connected.send_user_arrow("up", "cube-connect")
        connected.wait_for_text("LESS_LINE_0001")
        connected.write(b"q")
        connected.wait(timeout=5)
    finally:
        if connected.process.poll() is None:
            connected.write(b"\x1cd")
        connected.terminate()

    harness.wait_for_cube_output(["ps"], "less-cube\ttty\tcompleted")
    harness.run_cube(["cleanup", "less-cube"])
    assert_manager_responsive(harness)


def run_desk_less_arrows(harness):
    launch_less_cube(harness, "less-desk")
    desk = harness.spawn_desk_pty([], rows=24, cols=80)
    try:
        desk.wait_for_text("LESS_LINE_0001")
        desk.output.clear()

        desk.send_user_arrow("down", "desk")
        desk.wait_for_text("LESS_LINE_0002")
        desk.output.clear()

        desk.send_user_arrow("up", "desk")
        desk.wait_for_text("LESS_LINE_0001")
        desk.write(b"\x18q")
        desk.wait(timeout=5)
    finally:
        if desk.process.poll() is None:
            desk.write(b"\x18q")
            time.sleep(0.1)
        desk.terminate()

    output = harness.run_cube(["ps"])
    if "less-desk\ttty\trunning" not in output:
        raise AssertionError(
            "desk should quit without killing the less cube; "
            f"visible={visible_text(desk)!r}\nps:\n{output}"
        )
    harness.run_cube(["kill", "--cleanup", "less-desk"])
    assert_manager_responsive(harness)


def main():
    if shutil.which("less") is None:
        print("SKIP: less is not installed")
        return 0

    with CubicleEndUserHarness() as harness:
        harness.run_cube(["workspace", "create", "EndUser"])
        harness.run_cube(["workspace", "EndUser"])
        run_cube_connect_less_arrows(harness)
        run_desk_less_arrows(harness)
    return 0


if __name__ == "__main__":
    sys.exit(main())
