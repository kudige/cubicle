#!/usr/bin/env python3
import argparse
import contextlib
import fcntl
import os
import select
import signal
import socket
import struct
import sys
import termios


def read_socket_response(client):
    response = b""
    while True:
        chunk = client.recv(4096)
        if not chunk:
            break
        response += chunk
    return response


def send_request(socket_path, command, shutdown_write=True):
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        client.connect(socket_path)
        client.sendall(command.encode("utf-8") + b"\n")
        if shutdown_write:
            client.shutdown(socket.SHUT_WR)
        return read_socket_response(client)
    finally:
        client.close()


def parse_payload_response(response):
    header, separator, payload = response.partition(b"\n")
    if separator == b"":
        raise RuntimeError("response did not include a payload header")
    fields = header.decode("utf-8").split()
    if len(fields) < 2 or fields[0] != "ok" or not fields[1].startswith("length="):
        raise RuntimeError(header.decode("utf-8", errors="replace"))
    return payload


def parse_metadata(metadata):
    values = {}
    for line in metadata.decode("utf-8").splitlines():
        key, separator, value = line.partition("=")
        if separator == "=":
            values[key] = value
    return values


def get_controller_mode(socket_path):
    response = send_request(socket_path, "metadata")
    metadata = parse_metadata(parse_payload_response(response))
    mode = metadata.get("mode")
    if not mode:
        raise RuntimeError("metadata did not include mode")
    return mode


def get_terminal_size():
    if not sys.stdout.isatty():
        return None
    size = fcntl.ioctl(sys.stdout.fileno(), termios.TIOCGWINSZ,
                       struct.pack("HHHH", 0, 0, 0, 0))
    rows, columns, _, _ = struct.unpack("HHHH", size)
    if rows == 0 or columns == 0:
        return None
    return rows, columns


def resize_controller(socket_path, rows, columns):
    response = send_request(socket_path, f"resize {rows} {columns}")
    if response != b"ok\n":
        raise RuntimeError(response.decode("utf-8", errors="replace").strip())


@contextlib.contextmanager
def maybe_raw_terminal(enabled):
    if not enabled or not sys.stdin.isatty() or not sys.stdout.isatty():
        yield
        return

    fd = sys.stdin.fileno()
    original = termios.tcgetattr(fd)
    raw = original[:]
    raw[0] &= ~(termios.BRKINT | termios.ICRNL | termios.INPCK |
                termios.ISTRIP | termios.IXON)
    raw[1] &= ~termios.OPOST
    raw[2] |= termios.CS8
    raw[3] &= ~(termios.ECHO | termios.ICANON | termios.IEXTEN |
                termios.ISIG)
    raw[6][termios.VMIN] = 1
    raw[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSAFLUSH, raw)
    try:
        yield
    finally:
        termios.tcsetattr(fd, termios.TCSAFLUSH, original)


def connect_attach(socket_path, command):
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        client.connect(socket_path)
        client.sendall(command.encode("utf-8") + b"\n")

        response = b""
        while b"\n" not in response:
            chunk = client.recv(4096)
            if not chunk:
                break
            response += chunk

        header, separator, payload = response.partition(b"\n")
        if separator == b"":
            raise RuntimeError("attach response did not include a header")
        if not header.startswith(b"ok "):
            raise RuntimeError(header.decode("utf-8", errors="replace"))
        return client, header, payload
    except Exception:
        client.close()
        raise


def write_all(fd, data):
    view = memoryview(data)
    while view:
        written = os.write(fd, view)
        view = view[written:]


def send_command(socket_path, command):
    if (command.startswith("attach stdout ") or
        command.startswith("attach stderr ") or
        command.startswith("attach out ") or
        command.startswith("attach err ")):
        client, header, payload = connect_attach(socket_path, command)
        try:
            write_all(sys.stdout.fileno(), header + b"\n")
            if payload:
                write_all(sys.stdout.fileno(), payload)
            while True:
                chunk = client.recv(4096)
                if not chunk:
                    break
                write_all(sys.stdout.fileno(), chunk)
        finally:
            client.close()
        return

    response = send_request(socket_path, command,
                            shutdown_write=True)
    sys.stdout.buffer.write(response)
    sys.stdout.buffer.flush()


def attach_stdin(socket_path, command):
    mode = get_controller_mode(socket_path)
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        client.connect(socket_path)
        client.sendall(command.encode("utf-8") + b"\n")

        response = b""
        while not response.endswith(b"\n"):
            chunk = client.recv(4096)
            if not chunk:
                break
            response += chunk
        sys.stdout.buffer.write(response)
        sys.stdout.buffer.flush()

        with maybe_raw_terminal(mode == "tty"):
            while True:
                chunk = sys.stdin.buffer.read(4096)
                if not chunk:
                    break
                client.sendall(chunk)
    finally:
        client.close()


def attach_tty(socket_path, start):
    mode = get_controller_mode(socket_path)
    if mode != "tty":
        raise RuntimeError(f"attach tty requires mode=tty, got mode={mode}")

    terminal_size = get_terminal_size()
    if terminal_size is not None:
        resize_controller(socket_path, terminal_size[0], terminal_size[1])

    output_client = None
    input_client = None
    previous_winch = None
    resize_pending = False

    def handle_winch(_signal_number, _frame):
        nonlocal resize_pending
        resize_pending = True

    try:
        output_client, _, payload = connect_attach(socket_path, f"attach out {start}")
        input_client, _, _ = connect_attach(socket_path, "attach stdin")
        output_client.setblocking(False)

        if payload:
            write_all(sys.stdout.fileno(), payload)

        if sys.stdout.isatty():
            previous_winch = signal.getsignal(signal.SIGWINCH)
            signal.signal(signal.SIGWINCH, handle_winch)

        stdin_open = True
        with maybe_raw_terminal(True):
            while True:
                if resize_pending:
                    resize_pending = False
                    terminal_size = get_terminal_size()
                    if terminal_size is not None:
                        resize_controller(socket_path, terminal_size[0],
                                          terminal_size[1])

                read_fds = [output_client]
                if stdin_open:
                    read_fds.append(sys.stdin)
                ready, _, _ = select.select(read_fds, [], [], 0.2)
                if output_client in ready:
                    chunk = output_client.recv(4096)
                    if not chunk:
                        break
                    write_all(sys.stdout.fileno(), chunk)
                if stdin_open and sys.stdin in ready:
                    chunk = os.read(sys.stdin.fileno(), 4096)
                    if not chunk:
                        stdin_open = False
                        with contextlib.suppress(OSError):
                            input_client.shutdown(socket.SHUT_WR)
                    else:
                        input_client.sendall(chunk)
    finally:
        if previous_winch is not None:
            signal.signal(signal.SIGWINCH, previous_winch)
        if input_client is not None:
            input_client.close()
        if output_client is not None:
            output_client.close()


def main():
    parser = argparse.ArgumentParser(
        description="Send a command to a Cubicle controller control socket."
    )
    parser.add_argument(
        "socket_path",
        help="Path to the controller Unix socket, e.g. /tmp/cubicle-run/control.sock.",
    )

    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("status")
    subparsers.add_parser("metadata")
    subparsers.add_parser("mode")
    subparsers.add_parser("terminate")

    events_parser = subparsers.add_parser("events-after")
    events_parser.add_argument("sequence", type=int)
    events_parser.add_argument("limit", type=int)

    signal_parser = subparsers.add_parser("signal")
    signal_parser.add_argument("number", type=int)

    resize_parser = subparsers.add_parser("resize")
    resize_parser.add_argument("rows", type=int)
    resize_parser.add_argument("columns", type=int)

    read_parser = subparsers.add_parser("read")
    read_parser.add_argument("stream", choices=["stdout", "stderr", "out", "err"])
    read_parser.add_argument("start", type=int)
    read_parser.add_argument("length", type=int)

    attach_parser = subparsers.add_parser("attach")
    attach_parser.add_argument("stream", choices=["stdin", "stdout", "stderr", "in", "out", "err", "tty"])
    attach_parser.add_argument("start", type=int, nargs="?")

    raw_parser = subparsers.add_parser("raw")
    raw_parser.add_argument("line", help="Raw one-line control command to send.")

    args = parser.parse_args()

    if args.command == "status":
        command = "status"
    elif args.command == "metadata":
        command = "metadata"
    elif args.command == "mode":
        try:
            print(get_controller_mode(args.socket_path))
        except RuntimeError as error:
            parser.error(str(error))
        return
    elif args.command == "terminate":
        command = "terminate"
    elif args.command == "events-after":
        command = f"events after {args.sequence} {args.limit}"
    elif args.command == "signal":
        command = f"signal {args.number}"
    elif args.command == "resize":
        command = f"resize {args.rows} {args.columns}"
    elif args.command == "read":
        command = f"read {args.stream} {args.start} {args.length}"
    elif args.command == "attach":
        if args.stream == "tty":
            try:
                attach_tty(args.socket_path, args.start or 0)
            except RuntimeError as error:
                parser.error(str(error))
            return
        if args.stream in ("stdin", "in"):
            if args.start is not None:
                parser.error("attach stdin does not take a start offset")
            command = f"attach {args.stream}"
            attach_stdin(args.socket_path, command)
            return
        if args.start is None:
            parser.error("attach stdout/stderr requires a start offset")
        command = f"attach {args.stream} {args.start}"
    else:
        command = args.line

    send_command(args.socket_path, command)


if __name__ == "__main__":
    main()
