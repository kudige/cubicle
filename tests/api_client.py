#!/usr/bin/env python3
import argparse
import json
import socket
import struct
import sys
import uuid


PROTOCOL_MAJOR = 0
PROTOCOL_MINOR = 1
DEFAULT_SESSION_ID = "local-session"


def read_exact(sock, byte_count):
    data = b""
    while len(data) < byte_count:
        chunk = sock.recv(byte_count - len(data))
        if not chunk:
            raise RuntimeError("short response")
        data += chunk
    return data


def send_request(socket_path, request, timeout):
    payload = json.dumps(request, separators=(",", ":")).encode("utf-8")
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(timeout)
        client.connect(socket_path)
        client.sendall(struct.pack("!I", len(payload)) + payload)
        header = read_exact(client, 4)
        length = struct.unpack("!I", header)[0]
        response = read_exact(client, length)
    return json.loads(response.decode("utf-8"))


def load_params(params_text):
    try:
        params = json.loads(params_text)
    except json.JSONDecodeError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from exc
    if not isinstance(params, dict):
        raise argparse.ArgumentTypeError("params must be a JSON object")
    return params


def call_api(args, method, params):
    request = {
        "protocol_major": PROTOCOL_MAJOR,
        "protocol_minor": PROTOCOL_MINOR,
        "request_id": args.request_id or str(uuid.uuid4()),
        "session_id": args.session_id,
        "method": method,
        "params": params,
    }
    response = send_request(args.socket_path, request, args.timeout)
    if args.raw:
        print(json.dumps(response, separators=(",", ":"), sort_keys=True))
    else:
        print(json.dumps(response, sort_keys=True))
    if not args.allow_error and not response.get("success", False):
        return 1
    return 0


def params_for_command(args):
    command = args.command
    if command == "call":
        return args.method, args.params
    if command == "ping":
        return "manager.ping", {}
    if command == "status":
        return "manager.status", {}
    if command == "shutdown":
        return "manager.shutdown", {}
    if command == "workspace-create":
        return "workspace.create", {"name": args.name}
    if command == "workspace-get":
        return "workspace.get", {"workspace": args.workspace}
    if command == "workspace-list":
        return "workspace.list", {}
    if command == "process-get":
        params = {"process": args.process}
        if args.workspace is not None:
            params["workspace_id"] = args.workspace
        return "process.get", params
    if command == "process-list":
        params = {}
        if args.workspace is not None:
            params["workspace_id"] = args.workspace
        return "process.list", params
    if command == "process-start":
        params = {
            "workspace_id": args.workspace,
            "mode": args.mode,
            "stdin_policy": args.stdin_policy,
            "argv": args.argv,
        }
        if args.friendly_name is not None:
            params["friendly_name"] = args.friendly_name
        if args.tty_rows is not None:
            params["tty_rows"] = args.tty_rows
        if args.tty_cols is not None:
            params["tty_cols"] = args.tty_cols
        return "process.start", params
    if command == "process-signal":
        return "process.signal", {
            "process_id": args.process,
            "signal_number": args.signal,
        }
    if command == "process-terminate":
        return "process.terminate", {
            "process_id": args.process,
            "grace_period_ms": args.grace_period_ms,
            "force_after_grace": args.force_after_grace,
        }
    if command == "workspace-key-add":
        return "workspace.key.add", {
            "workspace_id": args.workspace,
            "public_key": args.public_key,
            "label": args.label,
            "capabilities": args.capabilities,
        }
    if command == "workspace-key-list":
        return "workspace.key.list", {"workspace_id": args.workspace}
    if command == "workspace-key-revoke":
        return "workspace.key.revoke", {
            "workspace_id": args.workspace,
            "key_id": args.key,
        }
    if command == "attachment-request":
        return "attachment.request", {
            "process_id": args.process,
            "channels": args.channels,
            "mode": args.mode,
            "stdout_offset": args.stdout_offset,
            "stderr_offset": args.stderr_offset,
            "tty_offset": args.tty_offset,
            "rows": args.rows,
            "cols": args.cols,
        }
    if command == "events-list":
        params = {
            "after_sequence": args.after,
            "limit": args.limit,
        }
        if args.workspace is not None:
            params["workspace_id"] = args.workspace
        if args.process is not None:
            params["process_id"] = args.process
        return "events.list", params
    if command == "read-output":
        return "process.read_output", {
            "process_id": args.process,
            "stream": args.stream,
            "offset": args.offset,
            "maximum_length": args.maximum_length,
        }
    raise RuntimeError(f"unhandled command: {command}")


def build_parser():
    parser = argparse.ArgumentParser(
        description="Send Cubicle API requests over a Unix socket.")
    parser.add_argument("socket_path")
    parser.add_argument("--allow-error", action="store_true",
                        help="exit successfully for API error responses")
    parser.add_argument("--raw", action="store_true",
                        help="print compact JSON")
    parser.add_argument("--request-id",
                        help="request id to include in the API envelope")
    parser.add_argument("--session-id", default=DEFAULT_SESSION_ID,
                        help="session id to include in the API envelope")
    parser.add_argument("--timeout", type=float, default=5.0,
                        help="socket timeout in seconds")

    subparsers = parser.add_subparsers(dest="command", required=True)

    call_parser = subparsers.add_parser("call")
    call_parser.add_argument("method")
    call_parser.add_argument("params", nargs="?", default={},
                             type=load_params)

    subparsers.add_parser("ping")
    subparsers.add_parser("status")
    subparsers.add_parser("shutdown")

    workspace_create = subparsers.add_parser("workspace-create")
    workspace_create.add_argument("name")

    workspace_get = subparsers.add_parser("workspace-get")
    workspace_get.add_argument("workspace")

    subparsers.add_parser("workspace-list")

    process_get = subparsers.add_parser("process-get")
    process_get.add_argument("process")
    process_get.add_argument("--workspace")

    process_list = subparsers.add_parser("process-list")
    process_list.add_argument("--workspace")

    process_start = subparsers.add_parser("process-start")
    process_start.add_argument("--workspace", required=True)
    process_start.add_argument("--friendly-name")
    process_start.add_argument("--mode", choices=("stream", "tty"),
                               default="stream")
    process_start.add_argument("--stdin-policy", choices=("open", "eof"),
                               default="open")
    process_start.add_argument("--tty-rows", type=int)
    process_start.add_argument("--tty-cols", type=int)
    process_start.add_argument("argv", nargs="+")

    process_signal = subparsers.add_parser("process-signal")
    process_signal.add_argument("process")
    process_signal.add_argument("signal", type=int)

    process_terminate = subparsers.add_parser("process-terminate")
    process_terminate.add_argument("process")
    process_terminate.add_argument("--grace-period-ms", type=int, default=0)
    process_terminate.add_argument("--force-after-grace",
                                   action="store_true")

    workspace_key_add = subparsers.add_parser("workspace-key-add")
    workspace_key_add.add_argument("workspace")
    workspace_key_add.add_argument("public_key")
    workspace_key_add.add_argument("--label", default="")
    workspace_key_add.add_argument("--capabilities", type=int, default=0)

    workspace_key_list = subparsers.add_parser("workspace-key-list")
    workspace_key_list.add_argument("workspace")

    workspace_key_revoke = subparsers.add_parser("workspace-key-revoke")
    workspace_key_revoke.add_argument("workspace")
    workspace_key_revoke.add_argument("key")

    attachment_request = subparsers.add_parser("attachment-request")
    attachment_request.add_argument("process")
    attachment_request.add_argument("--channels", type=int, required=True)
    attachment_request.add_argument("--mode", choices=("observer",
                                                       "interactive"),
                                    default="observer")
    attachment_request.add_argument("--stdout-offset", type=int, default=0)
    attachment_request.add_argument("--stderr-offset", type=int, default=0)
    attachment_request.add_argument("--tty-offset", type=int, default=0)
    attachment_request.add_argument("--rows", type=int, default=0)
    attachment_request.add_argument("--cols", type=int, default=0)

    events_list = subparsers.add_parser("events-list")
    events_list.add_argument("--workspace")
    events_list.add_argument("--process")
    events_list.add_argument("--after", type=int, default=0)
    events_list.add_argument("--limit", type=int, default=100)

    read_output = subparsers.add_parser("read-output")
    read_output.add_argument("process")
    read_output.add_argument("--stream", choices=("stdout", "stderr"),
                             required=True)
    read_output.add_argument("--offset", type=int, required=True)
    read_output.add_argument("--max", dest="maximum_length", type=int,
                             default=65536)

    return parser


def main():
    parser = build_parser()
    args = parser.parse_args()
    try:
        method, params = params_for_command(args)
        return call_api(args, method, params)
    except Exception as exc:
        print(f"api_client.py: error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
