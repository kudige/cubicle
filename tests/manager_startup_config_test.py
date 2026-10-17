import os
from pathlib import Path
import socket
import ssl
import subprocess
import tempfile
import time


def check_startup(mode, tls):
    with tempfile.TemporaryDirectory(prefix="cubicle-start-") as root:
        root = Path(root)
        runtime = root / "run"
        local_socket = runtime / "manager.sock"
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        config = root / "config.cfg"
        listener = f"listen=tls://127.0.0.1:{port}\n" if tls else ""
        config.write_text(
            f"[installation]\nbindir={Path(os.environ['CUBICLE_MANAGER']).parent}\n"
            f"[manager]\nstate_dir={root / 'state'}\nruntime_dir={runtime}\n"
            f"log_dir={root / 'log'}\n{listener}"
            f"[client]\nmanager=unix://{local_socket}\n[cube]\nautomanager=true\n"
        )
        env = dict(os.environ, CUBICLE_CONFIG=str(config),
                   XDG_STATE_HOME=str(root / "client-state"))
        env.pop("CUBICLE_MANAGER_SOCKET", None)
        command = [os.environ["CUBICLE_MANAGER"]]
        if mode == "explicit":
            command.append("--daemon")
        elif mode == "auto":
            command = [os.environ["CUBE"], "workspace", "list"]
        try:
            subprocess.run(command, env=env, check=True, timeout=10,
                           stdout=subprocess.DEVNULL)
            for _ in range(100):
                if local_socket.exists():
                    break
                time.sleep(0.05)
            subprocess.run(
                ["python3", os.environ["CUBICLE_API_CLIENT"], str(local_socket), "ping"],
                env=env, check=True, timeout=10, stdout=subprocess.DEVNULL,
            )
            if tls:
                context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
                context.check_hostname = False
                context.verify_mode = ssl.CERT_NONE
                with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
                    with context.wrap_socket(sock, server_hostname="localhost") as secured:
                        assert secured.version(), "TLS handshake failed"
        finally:
            if local_socket.exists():
                subprocess.run(
                    ["python3", os.environ["CUBICLE_API_CLIENT"], str(local_socket), "shutdown"],
                    env=env, check=True, timeout=10, stdout=subprocess.DEVNULL,
                )
                for _ in range(100):
                    if not local_socket.exists():
                        break
                    time.sleep(0.05)


for mode, tls in [("default", False), ("default", True), ("explicit", True), ("auto", True)]:
    check_startup(mode, tls)
