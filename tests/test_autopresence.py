#!/usr/bin/env python3
"""
Test Auto-Presence functionality:
Verifies that when a game without native Discord RPC is launched,
the plugin detects the AppID, resolves the game title, connects to Discord IPC,
and transmits a SET_ACTIVITY frame with game title, elapsed timer, and Steam CDN art.
"""

import os
import socket
import struct
import json
import subprocess
import tempfile
import time
import sys

def test_game(appid, expected_title, extra_env=None, expected_state=None):
    with tempfile.TemporaryDirectory(dir="/tmp") as tmpdir:
        sock_path = os.path.join(tmpdir, "discord-ipc-0")
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(sock_path)
        server.listen(1)

        # Set up environment pointing to fake discord socket
        env = dict(os.environ)
        env["NOTPROTON_DISCORD_SOCKET_DIR"] = tmpdir
        if appid:
            env["STEAM_COMPAT_APP_ID"] = appid
        env["DYLD_INSERT_LIBRARIES"] = os.path.abspath("build/discord_bridge.dylib")
        if extra_env:
            env.update(extra_env)

        # Compile a tiny Mach-O binary named 'wine' to trigger dylib constructor
        fake_wine = os.path.join(tmpdir, "wine")
        subprocess.run(["cc", "-x", "c", "-o", fake_wine, "-"], input=b"#include <unistd.h>\nint main(){ sleep(10); return 0; }\n", check=True)

        proc = subprocess.Popen([fake_wine], env=env)

        try:
            server.settimeout(6.0)
            client, _ = server.accept()
            client.settimeout(5.0)

            # Receive Handshake
            hdr = client.recv(8)
            assert len(hdr) == 8, f"Expected 8 bytes header, got {len(hdr)}"
            op, length = struct.unpack("<II", hdr)
            assert op == 0, f"Expected handshake opcode 0, got {op}"
            payload = json.loads(client.recv(length).decode("utf-8"))
            assert "client_id" in payload, "Missing client_id in handshake"

            # Reply to Handshake with READY frame
            ready = json.dumps({"cmd": "DISPATCH", "evt": "READY", "data": {}}).encode("utf-8")
            client.sendall(struct.pack("<II", 1, len(ready)) + ready)

            # Receive SET_ACTIVITY frame
            hdr = client.recv(8)
            assert len(hdr) == 8, f"Expected 8 bytes header, got {len(hdr)}"
            op, length = struct.unpack("<II", hdr)
            assert op == 1, f"Expected frame opcode 1, got {op}"
            act_payload = json.loads(client.recv(length).decode("utf-8"))
            
            assert act_payload.get("cmd") == "SET_ACTIVITY"
            activity = act_payload["args"]["activity"]
            assert activity["name"] == expected_title, f"Expected '{expected_title}', got {activity['name']}"
            if expected_state is not None:
                assert activity.get("state") == expected_state, f"Expected state '{expected_state}', got '{activity.get('state')}'"
            check_id = appid or (extra_env.get("NOTPROTON_DISCORD_APPID") if extra_env else None)
            if check_id:
                assert check_id in activity["assets"]["large_image"], f"Expected AppID in image URL, got {activity['assets']['large_image']}"
            print(f"  ok    Auto-Presence verified for {expected_title}")

        finally:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except Exception:
                proc.kill()
            server.close()

def test_disabled(extra_env):
    with tempfile.TemporaryDirectory(dir="/tmp") as tmpdir:
        sock_path = os.path.join(tmpdir, "discord-ipc-0")
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(sock_path)
        server.listen(1)

        env = dict(os.environ)
        env["NOTPROTON_DISCORD_SOCKET_DIR"] = tmpdir
        env["STEAM_COMPAT_APP_ID"] = "205100"
        env["DYLD_INSERT_LIBRARIES"] = os.path.abspath("build/discord_bridge.dylib")
        env.update(extra_env)

        fake_wine = os.path.join(tmpdir, "wine")
        subprocess.run(["cc", "-x", "c", "-o", fake_wine, "-"], input=b"#include <unistd.h>\nint main(){ sleep(2); return 0; }\n", check=True)

        proc = subprocess.Popen([fake_wine], env=env)
        server.settimeout(1.5)
        connected = False
        try:
            client, _ = server.accept()
            connected = True
            client.close()
        except socket.timeout:
            connected = False
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=1)
            except Exception:
                proc.kill()
            server.close()

        assert not connected, f"Expected no connection when {extra_env} is set, but socket connected"
        print(f"  ok    Disabled flag verified for {extra_env}")

def main():
    test_game("205100", "Dishonored")
    test_game("312520", "Rain World")
    test_game("632360", "Risk of Rain 2")
    # Test overrides
    test_game("205100", "Dishonored (Custom Mod)", extra_env={"NOTPROTON_DISCORD_TITLE": "Dishonored (Custom Mod)", "NOTPROTON_DISCORD_FORCE_AUTO": "1"})
    test_game("205100", "Dishonored", extra_env={"NOTPROTON_DISCORD_STATE": "Stealth Run", "NOTPROTON_DISCORD_FORCE_AUTO": "1"}, expected_state="Stealth Run")
    test_game(None, "Dishonored", extra_env={"NOTPROTON_DISCORD_APPID": "205100", "NOTPROTON_DISCORD_FORCE_AUTO": "1"})
    # Test disabled flags
    test_disabled({"NOTPROTON_DISCORD_DISABLE": "1"})
    test_disabled({"NOTPROTON_DISCORD_DISABLE_AUTO": "1"})
    print("==> Auto-Presence test suite passed successfully!")

if __name__ == "__main__":
    main()
