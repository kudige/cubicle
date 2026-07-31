#!/usr/bin/env python3
import argparse
import socket
import sys


def send_command(socket_path, command, forward_stdin=False):
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        client.connect(socket_path)
        client.sendall(command.encode("utf-8") + b"\n")

        if forward_stdin:
            response = b""
            while not response.endswith(b"\n"):
                chunk = client.recv(4096)
                if not chunk:
                    break
                response += chunk
            sys.stdout.buffer.write(response)
            sys.stdout.buffer.flush()

            while True:
                chunk = sys.stdin.buffer.read(4096)
                if not chunk:
                    break
                client.sendall(chunk)
            return

        if not command.startswith("attach "):
            client.shutdown(socket.SHUT_WR)

        while True:
            chunk = client.recv(4096)
            if not chunk:
                break
            sys.stdout.buffer.write(chunk)
            sys.stdout.buffer.flush()
    finally:
        client.close()


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
    attach_parser.add_argument("stream", choices=["stdin", "stdout", "stderr", "in", "out", "err"])
    attach_parser.add_argument("start", type=int, nargs="?")

    raw_parser = subparsers.add_parser("raw")
    raw_parser.add_argument("line", help="Raw one-line control command to send.")

    args = parser.parse_args()

    if args.command == "status":
        command = "status"
    elif args.command == "metadata":
        command = "metadata"
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
        if args.stream in ("stdin", "in"):
            if args.start is not None:
                parser.error("attach stdin does not take a start offset")
            command = f"attach {args.stream}"
            send_command(args.socket_path, command, forward_stdin=True)
            return
        if args.start is None:
            parser.error("attach stdout/stderr requires a start offset")
        command = f"attach {args.stream} {args.start}"
    else:
        command = args.line

    send_command(args.socket_path, command)


if __name__ == "__main__":
    main()
