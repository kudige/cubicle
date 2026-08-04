#!/usr/bin/env python3
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import uuid


def read_exact(sock, byte_count):
    data = b""
    while len(data) < byte_count:
        chunk = sock.recv(byte_count - len(data))
        if not chunk:
            raise AssertionError("socket closed while reading response")
        data += chunk
    return data


def send_frame(sock, method, params=None):
    request = {
        "protocol_major": 0,
        "protocol_minor": 1,
        "request_id": str(uuid.uuid4()),
        "session_id": "local-session",
        "method": method,
        "params": params or {},
    }
    payload = json.dumps(request, separators=(",", ":")).encode("utf-8")
    sock.sendall(struct.pack("!I", len(payload)) + payload)
    header = read_exact(sock, 4)
    length = struct.unpack("!I", header)[0]
    response = json.loads(read_exact(sock, length).decode("utf-8"))
    if not response.get("success", False):
        raise AssertionError(f"{method} failed: {response}")
    return response


def wait_for_ready(socket_path, api_client, env):
    for _ in range(100):
        if os.path.exists(socket_path):
            ping = subprocess.run(
                [sys.executable, api_client, socket_path, "ping"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                env=env,
            )
            if ping.returncode == 0:
                return
        time.sleep(0.05)
    raise AssertionError("manager did not become ready")


def start_manager(manager, tmpdir, api_client, max_clients=None):
    socket_path = os.path.join(tmpdir, "manager.sock")
    state_dir = os.path.join(tmpdir, "state")
    env = os.environ.copy()
    env["XDG_CONFIG_HOME"] = os.path.join(tmpdir, "config")
    env["XDG_RUNTIME_DIR"] = os.path.join(tmpdir, "runtime")
    env["XDG_STATE_HOME"] = os.path.join(tmpdir, "xdg-state")
    os.makedirs(env["XDG_CONFIG_HOME"], exist_ok=True)
    os.makedirs(env["XDG_RUNTIME_DIR"], exist_ok=True)
    os.makedirs(env["XDG_STATE_HOME"], exist_ok=True)

    command = [
        manager,
        "--state-dir",
        state_dir,
        "daemon",
        "--foreground",
        "--control-socket",
        socket_path,
        "--event-interval-ms",
        "50",
    ]
    if max_clients is not None:
        command.extend(["--max-clients", str(max_clients)])

    proc = subprocess.Popen(
        command,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        env=env,
    )
    wait_for_ready(socket_path, api_client, env)
    return proc, socket_path, env


def stop_manager(proc, socket_path, api_client, env):
    subprocess.run(
        [sys.executable, api_client, socket_path, "shutdown"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        env=env,
    )
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


def connect_raw(socket_path):
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(2)
    sock.connect(socket_path)
    return sock


def run_api(api_client, socket_path, command, env, expect_success=True):
    result = subprocess.run(
        [sys.executable, api_client, "--timeout", "1", socket_path, command],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=env,
    )
    if expect_success and result.returncode != 0:
        raise AssertionError(
            f"api_client {command} failed\nstdout={result.stdout}\nstderr={result.stderr}"
        )
    return result


def assert_idle_client_does_not_starve_manager(manager, api_client):
    tmpdir = tempfile.mkdtemp(prefix="cubicle-manager-parallel.")
    proc = None
    idle = None
    persistent = None
    try:
        proc, socket_path, env = start_manager(manager, tmpdir, api_client)
        idle = connect_raw(socket_path)

        run_api(api_client, socket_path, "ping", env)
        run_api(api_client, socket_path, "status", env)

        send_frame(idle, "manager.ping")

        persistent = connect_raw(socket_path)
        send_frame(persistent, "manager.ping")
        run_api(api_client, socket_path, "status", env)
        send_frame(persistent, "manager.status")

        stop_manager(proc, socket_path, api_client, env)
        proc = None
        for _ in range(50):
            if not os.path.exists(socket_path):
                break
            time.sleep(0.05)
        if os.path.exists(socket_path):
            raise AssertionError("manager did not remove socket after shutdown")
    finally:
        for sock in (idle, persistent):
            if sock is not None:
                sock.close()
        if proc is not None:
            stop_manager(proc, os.path.join(tmpdir, "manager.sock"), api_client, env)
        shutil.rmtree(tmpdir, ignore_errors=True)


def assert_max_clients_is_enforced(manager, api_client):
    tmpdir = tempfile.mkdtemp(prefix="cubicle-manager-max-clients.")
    proc = None
    idle = None
    try:
        proc, socket_path, env = start_manager(manager, tmpdir, api_client, max_clients=1)
        idle = connect_raw(socket_path)

        rejected = run_api(api_client, socket_path, "ping", env, expect_success=False)
        if rejected.returncode == 0:
            raise AssertionError("second client unexpectedly succeeded at max capacity")

        send_frame(idle, "manager.ping")
        idle.close()
        idle = None

        for _ in range(50):
            recovered = run_api(api_client, socket_path, "ping", env, expect_success=False)
            if recovered.returncode == 0:
                break
            time.sleep(0.05)
        else:
            raise AssertionError("manager did not accept clients after capacity freed")
    finally:
        if idle is not None:
            idle.close()
        if proc is not None:
            stop_manager(proc, os.path.join(tmpdir, "manager.sock"), api_client, env)
        shutil.rmtree(tmpdir, ignore_errors=True)


def main():
    manager = os.environ["CUBICLE_MANAGER"]
    api_client = os.environ["CUBICLE_API_CLIENT"]
    assert_idle_client_does_not_starve_manager(manager, api_client)
    assert_max_clients_is_enforced(manager, api_client)


if __name__ == "__main__":
    main()
