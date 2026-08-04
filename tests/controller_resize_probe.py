#!/usr/bin/env python3
import argparse
import json
import socket
import struct
import sys
import time
import uuid
from pathlib import Path


CHANNEL_STDIN = 1
CHANNEL_STDOUT = 2
CHANNEL_TTY = 8


def read_exact(sock, size):
    data = b""
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise RuntimeError("controller closed the connection")
        data += chunk
    return data


def controller_call(socket_path, method, params, timeout, quiet=False):
    request = {
        "protocol_major": 0,
        "protocol_minor": 1,
        "request_id": str(uuid.uuid4()),
        "session_id": "resize-probe",
        "method": method,
        "params": params,
    }
    payload = json.dumps(request, separators=(",", ":")).encode("utf-8")
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.settimeout(timeout)
        sock.connect(socket_path)
        sock.sendall(struct.pack("!I", len(payload)) + payload)
        length = struct.unpack("!I", read_exact(sock, 4))[0]
        response = json.loads(read_exact(sock, length).decode("utf-8"))
    if not quiet:
        print(json.dumps(response, sort_keys=True))
    if not response.get("success", False):
        raise RuntimeError(f"{method} failed")
    return response


def drain_tty(socket_path, offset, timeout, duration, interval, maximum_length):
    deadline = time.monotonic() + duration
    reads = 0
    bytes_read = 0
    end_of_stream = False
    while time.monotonic() < deadline:
        response = controller_call(
            socket_path,
            "controller.read",
            {
                "stream": "tty",
                "offset": offset,
                "maximum_length": maximum_length,
            },
            timeout,
            quiet=True,
        )
        result = response["result"]
        data = result.get("data", "")
        offset = int(result["next_offset"])
        end_of_stream = bool(result["end_of_stream"])
        reads += 1
        bytes_read += len(data.encode("utf-8"))
        if not data:
            time.sleep(interval)
    print(
        json.dumps(
            {
                "drain_reads": reads,
                "drain_bytes": bytes_read,
                "next_offset": offset,
                "end_of_stream": end_of_stream,
            },
            sort_keys=True,
        )
    )
    return offset


def socket_for_process(runtime_dir, process_id):
    return str(Path(runtime_dir) / "controllers" / process_id / "control.sock")


def tail_file(path, lines):
    try:
        data = Path(path).read_text(encoding="utf-8", errors="replace")
    except FileNotFoundError:
        return
    for line in data.splitlines()[-lines:]:
        print(line)


def main():
    parser = argparse.ArgumentParser(
        description="Attach to a controller and send resize events without replaying output."
    )
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument("--socket", help="Controller control socket path.")
    target.add_argument("--process-id", help="Process id under runtime-dir/controllers.")
    parser.add_argument("--runtime-dir", help="Manager runtime dir for --process-id.")
    parser.add_argument("--rows", type=int, default=40)
    parser.add_argument("--cols", type=int, default=120)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--delay", type=float, default=0.2)
    parser.add_argument("--attach", action="store_true")
    parser.add_argument("--detach", action="store_true")
    parser.add_argument("--drain-seconds", type=float, default=0.0)
    parser.add_argument("--read-interval", type=float, default=0.05)
    parser.add_argument("--maximum-read", type=int, default=8192)
    parser.add_argument("--snapshot", action="store_true")
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--events-log")
    parser.add_argument("--tail", type=int, default=30)
    args = parser.parse_args()

    if args.socket:
        socket_path = args.socket
    else:
        if not args.runtime_dir:
            parser.error("--runtime-dir is required with --process-id")
        socket_path = socket_for_process(args.runtime_dir, args.process_id)

    print(f"# controller={socket_path}")
    controller_call(socket_path, "controller.status", {}, args.timeout)
    if args.snapshot:
        controller_call(socket_path, "controller.snapshot", {}, args.timeout)
    if args.attach:
        response = controller_call(
            socket_path,
            "controller.attach",
            {
                "token": "local:resize-probe",
                "channels": CHANNEL_STDIN | CHANNEL_STDOUT | CHANNEL_TTY,
                "mode": "interactive",
            },
            args.timeout,
        )
        tty_offset = int(response["result"].get("tty_offset", 0))
    else:
        tty_offset = int(controller_call(
            socket_path, "controller.status", {}, args.timeout
        )["result"].get("tty_offset", 0))
    for _ in range(args.repeat):
        controller_call(
            socket_path,
            "controller.resize",
            {"rows": args.rows, "columns": args.cols},
            args.timeout,
        )
        time.sleep(args.delay)
    if args.drain_seconds > 0:
        tty_offset = drain_tty(
            socket_path,
            tty_offset,
            args.timeout,
            args.drain_seconds,
            args.read_interval,
            args.maximum_read,
        )
    controller_call(socket_path, "controller.status", {}, args.timeout)
    if args.detach:
        controller_call(socket_path, "controller.detach", {}, args.timeout)
    if args.events_log:
        print(f"# tail {args.events_log}")
        tail_file(args.events_log, args.tail)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"resize-probe: {exc}", file=sys.stderr)
        sys.exit(1)
