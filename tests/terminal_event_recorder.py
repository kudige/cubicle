#!/usr/bin/env python3
import argparse
import json
import os
import select
import signal
import sys
import termios
import time
import tty


CSI = "\x1b["


def terminal_size():
    size = os.get_terminal_size(sys.stdout.fileno())
    return size.lines, size.columns


def write_stdout(text):
    os.write(sys.stdout.fileno(), text.encode("utf-8"))


def draw_border(rows, columns):
    if rows < 2 or columns < 2:
        write_stdout(CSI + "2J" + CSI + "Htoo small")
        return

    top = "+" + ("-" * (columns - 2)) + "+"
    middle = "|" + (" " * (columns - 2)) + "|"
    status = " terminal_event_recorder "
    if len(status) + 2 < columns:
        top = "+" + status + ("-" * (columns - len(status) - 2)) + "+"

    parts = [CSI + "?25l", CSI + "2J", CSI + "H", top]
    for row in range(2, rows):
        parts.append(f"{CSI}{row};1H{middle}")
    parts.append(f"{CSI}{rows};1H+{('-' * (columns - 2))}+")
    parts.append(f"{CSI}2;3Hsize={rows}x{columns} exit=q")
    write_stdout("".join(parts))


def record_event(output, event, **fields):
    payload = {
        "time": time.time(),
        "event": event,
    }
    payload.update(fields)
    output.write(json.dumps(payload, sort_keys=True) + "\n")
    output.flush()


def byte_text(data):
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return None


def main():
    parser = argparse.ArgumentParser(
        description="Record TTY input and resize events as JSON lines."
    )
    parser.add_argument("--output", required=True, help="Path to write JSONL events.")
    parser.add_argument(
        "--exit-key",
        default="q",
        help="Single-byte key that exits the recorder. Defaults to q.",
    )
    args = parser.parse_args()

    if len(args.exit_key.encode("utf-8")) != 1:
        parser.error("--exit-key must encode to exactly one byte")
    exit_byte = args.exit_key.encode("utf-8")

    stdin_fd = sys.stdin.fileno()
    original = termios.tcgetattr(stdin_fd)
    resize_pending = False

    def handle_winch(_signal_number, _frame):
        nonlocal resize_pending
        resize_pending = True

    previous_winch = signal.getsignal(signal.SIGWINCH)
    signal.signal(signal.SIGWINCH, handle_winch)

    with open(args.output, "a", encoding="utf-8") as output:
        try:
            tty.setraw(stdin_fd)
            rows, columns = terminal_size()
            record_event(output, "start", rows=rows, columns=columns)
            draw_border(rows, columns)

            while True:
                if resize_pending:
                    resize_pending = False
                    rows, columns = terminal_size()
                    record_event(output, "resize", rows=rows, columns=columns)
                    draw_border(rows, columns)

                ready, _, _ = select.select([stdin_fd], [], [], 0.2)
                if not ready:
                    continue

                data = os.read(stdin_fd, 4096)
                if not data:
                    record_event(output, "eof")
                    break

                record_event(output, "input", hex=data.hex(), text=byte_text(data))
                if b"\x03" in data:
                    record_event(output, "control_c")
                if exit_byte in data:
                    record_event(output, "exit", reason="exit_key")
                    break
        finally:
            termios.tcsetattr(stdin_fd, termios.TCSAFLUSH, original)
            signal.signal(signal.SIGWINCH, previous_winch)
            write_stdout(CSI + "?25h" + CSI + "0m" + CSI + "H")


if __name__ == "__main__":
    main()
