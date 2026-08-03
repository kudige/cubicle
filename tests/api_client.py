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


def connect_endpoint(endpoint, timeout):
    if endpoint.startswith("tcp://"):
        authority = endpoint[len("tcp://"):]
        if authority.startswith("["):
            host, separator, port_text = authority[1:].partition("]:")
            if separator == "":
                raise ValueError("tcp endpoint must use tcp://host:port")
        else:
            host, separator, port_text = authority.rpartition(":")
            if separator == "":
                raise ValueError("tcp endpoint must use tcp://host:port")
        client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        client.settimeout(timeout)
        client.connect((host, int(port_text)))
        return client

    socket_path = endpoint
    if endpoint.startswith("unix://"):
        socket_path = endpoint[len("unix://"):]
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.settimeout(timeout)
    client.connect(socket_path)
    return client


def send_request(endpoint, request, timeout):
    payload = json.dumps(request, separators=(",", ":")).encode("utf-8")
    with connect_endpoint(endpoint, timeout) as client:
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
    response = send_request(args.endpoint, request, args.timeout)
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
    if command == "cleanup":
        params = {}
        if args.workspace is not None:
            params["workspace_id"] = args.workspace
        return "manager.cleanup", params
    if command == "shutdown":
        return "manager.shutdown", {}
    if command == "workspace-create":
        params = {"name": args.name}
        if args.directory is not None:
            params["directory"] = args.directory
        return "workspace.create", params
    if command == "workspace-get":
        return "workspace.get", {"workspace": args.workspace}
    if command == "workspace-list":
        return "workspace.list", {}
    if command == "workspace-rename":
        return "workspace.rename", {
            "workspace_id": args.workspace,
            "new_name": args.new_name,
        }
    if command == "workspace-stop":
        return "workspace.stop", {
            "workspace_id": args.workspace,
            "grace_period_ms": args.grace_period_ms,
            "force_after_grace": args.force_after_grace,
        }
    if command == "workspace-delete":
        return "workspace.delete", {
            "workspace_id": args.workspace,
            "stop_running_processes": args.stop_running_processes,
            "remove_retained_processes": args.remove_retained_processes,
        }
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
        if args.cwd is not None:
            params["cwd"] = args.cwd
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
    if command == "process-kill":
        return "process.kill", {"process_id": args.process}
    if command == "process-wait":
        return "process.wait", {
            "process_id": args.process,
            "timeout_ms": args.timeout_ms,
        }
    if command == "process-remove":
        return "process.remove", {"process_id": args.process}
    if command == "workspace-key-add":
        return "workspace.key.add", {
            "workspace_id": args.workspace,
            "public_key": args.public_key,
            "label": args.label,
            "capabilities": args.capabilities,
        }
    if command == "workspace-key-list":
        return "workspace.key.list", {"workspace_id": args.workspace}
    if command == "workspace-key-update":
        return "workspace.key.update", {
            "workspace_id": args.workspace,
            "key_id": args.key,
            "capabilities": args.capabilities,
        }
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
    if command == "controller-status":
        return "controller.status", {}
    if command == "controller-attach":
        return "controller.attach", {
            "token": args.token,
            "channels": args.channels,
            "mode": args.mode,
        }
    if command == "controller-read":
        return "controller.read", {
            "stream": args.stream,
            "offset": args.offset,
            "maximum_length": args.maximum_length,
        }
    if command == "controller-write":
        return "controller.write", {
            "channel": args.channel,
            "data": args.data,
        }
    if command == "controller-resize":
        return "controller.resize", {
            "rows": args.rows,
            "columns": args.columns,
        }
    if command == "controller-detach":
        return "controller.detach", {}
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
    if command == "events-subscribe":
        params = {
            "after_sequence": args.after,
            "limit": args.limit,
        }
        if args.workspace is not None:
            params["workspace_id"] = args.workspace
        if args.process is not None:
            params["process_id"] = args.process
        return "events.subscribe", params
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
        description="Send Cubicle API requests over a Unix or TCP endpoint.")
    parser.add_argument("endpoint",
                        help="Unix socket path, unix:///path, or tcp://host:port")
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
    cleanup = subparsers.add_parser("cleanup")
    cleanup.add_argument("--workspace")
    subparsers.add_parser("shutdown")

    workspace_create = subparsers.add_parser("workspace-create")
    workspace_create.add_argument("--dir", dest="directory")
    workspace_create.add_argument("name")

    workspace_get = subparsers.add_parser("workspace-get")
    workspace_get.add_argument("workspace")

    subparsers.add_parser("workspace-list")

    workspace_rename = subparsers.add_parser("workspace-rename")
    workspace_rename.add_argument("workspace")
    workspace_rename.add_argument("new_name")

    workspace_stop = subparsers.add_parser("workspace-stop")
    workspace_stop.add_argument("workspace")
    workspace_stop.add_argument("--grace-period-ms", type=int, default=0)
    workspace_stop.add_argument("--force-after-grace",
                                action="store_true")

    workspace_delete = subparsers.add_parser("workspace-delete")
    workspace_delete.add_argument("workspace")
    workspace_delete.add_argument("--stop-running-processes",
                                  action="store_true")
    workspace_delete.add_argument("--remove-retained-processes",
                                  action="store_true")

    process_get = subparsers.add_parser("process-get")
    process_get.add_argument("process")
    process_get.add_argument("--workspace")

    process_list = subparsers.add_parser("process-list")
    process_list.add_argument("--workspace")

    process_start = subparsers.add_parser("process-start")
    process_start.add_argument("--workspace", required=True)
    process_start.add_argument("--friendly-name")
    process_start.add_argument("--mode", choices=("stream", "tty", "term"),
                               default="stream")
    process_start.add_argument("--stdin-policy", choices=("open", "eof"),
                               default="open")
    process_start.add_argument("--cwd")
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

    process_kill = subparsers.add_parser("process-kill")
    process_kill.add_argument("process")

    process_wait = subparsers.add_parser("process-wait")
    process_wait.add_argument("process")
    process_wait.add_argument("--timeout-ms", type=int, default=0)

    process_remove = subparsers.add_parser("process-remove")
    process_remove.add_argument("process")

    workspace_key_add = subparsers.add_parser("workspace-key-add")
    workspace_key_add.add_argument("workspace")
    workspace_key_add.add_argument("public_key")
    workspace_key_add.add_argument("--label", default="")
    workspace_key_add.add_argument("--capabilities", type=int, default=0)

    workspace_key_list = subparsers.add_parser("workspace-key-list")
    workspace_key_list.add_argument("workspace")

    workspace_key_update = subparsers.add_parser("workspace-key-update")
    workspace_key_update.add_argument("workspace")
    workspace_key_update.add_argument("key")
    workspace_key_update.add_argument("--capabilities", type=int, required=True)

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

    subparsers.add_parser("controller-status")

    controller_attach = subparsers.add_parser("controller-attach")
    controller_attach.add_argument("token")
    controller_attach.add_argument("--channels", type=int, required=True)
    controller_attach.add_argument("--mode", choices=("observer",
                                                      "interactive"),
                                   default="observer")

    controller_read = subparsers.add_parser("controller-read")
    controller_read.add_argument("stream", choices=("stdout", "stderr",
                                                    "tty"))
    controller_read.add_argument("--offset", type=int, required=True)
    controller_read.add_argument("--max", dest="maximum_length", type=int,
                                 default=65536)

    controller_write = subparsers.add_parser("controller-write")
    controller_write.add_argument("data")
    controller_write.add_argument("--channel", choices=("stdin", "tty"),
                                  default="stdin")

    controller_resize = subparsers.add_parser("controller-resize")
    controller_resize.add_argument("rows", type=int)
    controller_resize.add_argument("columns", type=int)

    subparsers.add_parser("controller-detach")

    events_list = subparsers.add_parser("events-list")
    events_list.add_argument("--workspace")
    events_list.add_argument("--process")
    events_list.add_argument("--after", type=int, default=0)
    events_list.add_argument("--limit", type=int, default=100)

    events_subscribe = subparsers.add_parser("events-subscribe")
    events_subscribe.add_argument("--workspace")
    events_subscribe.add_argument("--process")
    events_subscribe.add_argument("--after", type=int, default=0)
    events_subscribe.add_argument("--limit", type=int, default=100)

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
