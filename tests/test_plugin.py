#!/usr/bin/env python3
"""Exercise the NotProton Discord RPC Plugin structure and dylib injection lifecycle."""
import contextlib
import json
import os
from pathlib import Path
import selectors
import socket
import subprocess
import sys
import tempfile
import threading
import time

plugin_conf_path = Path(__file__).resolve().parents[1] / "plugin/discord-rpc.conf"
bridge_dylib_path = Path(__file__).resolve().parents[1] / "build/discord_bridge.dylib"


def test_plugin_conf_schema():
    assert plugin_conf_path.exists(), f"Missing plugin conf: {plugin_conf_path}"
    lines = plugin_conf_path.read_text().splitlines()
    conf = {}
    current_section = None
    for line in lines:
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("[") and line.endswith("]"):
            current_section = line[1:-1]
            conf[current_section] = {}
            continue
        if "=" in line and current_section:
            k, v = line.split("=", 1)
            conf[current_section][k.strip()] = v.strip()

    assert "NOTPROTON_DISCORD_BRIDGE" in conf, "Missing [NOTPROTON_DISCORD_BRIDGE] section"
    sec = conf["NOTPROTON_DISCORD_BRIDGE"]
    assert sec.get("label") == "Discord Rich Presence", f"Unexpected label: {sec.get('label')}"
    assert sec.get("section") == "dev", f"Expected section=dev, got {sec.get('section')}"
    assert sec.get("on") == "1", f"Expected on=1, got {sec.get('on')}"
    assert sec.get("inject") == "1", f"Expected inject=1, got {sec.get('inject')}"
    assert "requires" in sec, "Missing 'requires' field"
    assert sec["requires"].endswith("discord_bridge.dylib"), "requires should point to discord_bridge.dylib"
    print("  ok    plugin conf schema matches NotProton specifications")


def test_dylib_lifecycle():
    assert bridge_dylib_path.exists(), f"Missing bridge dylib: {bridge_dylib_path}"

    with tempfile.TemporaryDirectory(prefix="np-plug-", dir="/tmp") as directory:
        tmp = Path(directory)
        mock_helper = tmp / "notproton-discord.exe"
        report_file = tmp / "report.json"
        
        # Create a mock helper script that pretends to be the Win32 helper
        mock_helper.write_text(f'''#!{sys.executable}
import json
import os
import socket
import sys
import time

port = int(os.environ.get("NOTPROTON_DISCORD_PORT", "0"))
token = os.environ.get("NOTPROTON_DISCORD_TOKEN", "")

with open({repr(str(report_file))}, "w") as f:
    json.dump({{"argv": sys.argv, "port": port, "token": token, "pid": os.getpid()}}, f)

if port and token:
    with socket.create_connection(("127.0.0.1", port)) as s:
        s.sendall(token.encode() + bytes([0])) # BRIDGE_CONTROL ready
        while True:
            time.sleep(0.1)
''')
        mock_helper.chmod(0o755)

        # Copy dylib next to mock helper so dladdr finds it
        test_dylib = tmp / "discord_bridge.dylib"
        test_dylib.write_bytes(bridge_dylib_path.read_bytes())

        # Create a mock discord unix socket
        discord_sock = tmp / "discord-ipc-0"
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(str(discord_sock))
        server.listen(1)

        # Compile a tiny test host binary that executes argv[1] (simulating Wine launching the helper)
        mock_wine = tmp / "wine"
        subprocess.run(["clang", "-O2", "-x", "c", "-", "-o", str(mock_wine)],
                       input=b'#include <unistd.h>\nint main(int argc, char **argv) { if (argc > 1) { execv(argv[1], &argv[1]); } sleep(2); return 0; }\n',
                       check=True)

        # Run dummy host process with DYLD_INSERT_LIBRARIES
        env = dict(os.environ,
                   DYLD_INSERT_LIBRARIES=str(test_dylib),
                   NOTPROTON_DISCORD_SOCKET_DIR=str(tmp),
                   WINELOADER=str(mock_wine))

        # Remove our own guards if present
        env.pop("NOTPROTON_DISCORD_ACTIVE", None)

        proc = subprocess.Popen(
            [str(mock_wine)],
            env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )

        # Wait for helper report
        deadline = time.monotonic() + 5
        while not report_file.exists() and time.monotonic() < deadline:
            time.sleep(0.05)

        assert report_file.exists(), "Helper was not spawned by dylib constructor"
        data = json.loads(report_file.read_text())
        assert data["port"] > 0, "Valid port was not assigned"
        assert len(data["token"]) == 64, "Valid 64-char token was not passed"
        helper_pid = data["pid"]

        # Wait for proc to finish
        proc.wait(timeout=5)
        assert proc.returncode == 0

        # Verify helper is reaped
        time.sleep(0.2)
        try:
            os.kill(helper_pid, 0)
        except ProcessLookupError:
            pass
        else:
            raise AssertionError("Mock helper process survived the host process termination")

        server.close()
        print("  ok    dylib constructor spawns helper and destructor reaps child cleanly")


if __name__ == "__main__":
    test_plugin_conf_schema()
    test_dylib_lifecycle()
    print("==> All NotProton Discord RPC plugin tests passed successfully!")
