#!/usr/bin/env python3
"""
Test Auto-Presence functionality:
Verifies that when a game without native Discord RPC is launched,
the plugin detects the AppID, resolves the game title, connects to Discord IPC,
and transmits a SET_ACTIVITY frame with game title, elapsed timer, and Steam CDN art.
"""

import os
import shutil
import socket
import struct
import json
import subprocess
import tempfile
import time
import sys

def test_game(appid, expected_title, extra_env=None, expected_state=None, expected_art_substr="library_600x900.jpg"):
    with tempfile.TemporaryDirectory(dir="/tmp") as tmpdir:
        sock_path = os.path.join(tmpdir, "discord-ipc-0")
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(sock_path)
        server.listen(1)

        # Create hermetic mock Steam environment
        steam_apps = os.path.join(tmpdir, "Library", "Application Support", "Steam", "steamapps")
        os.makedirs(steam_apps, exist_ok=True)
        target_id = appid or (extra_env.get("NOTPROTON_DISCORD_APPID") if extra_env else None)
        if target_id:
            manifest_path = os.path.join(steam_apps, f"appmanifest_{target_id}.acf")
            base_name = expected_title.split(" (")[0]
            with open(manifest_path, "w") as f:
                f.write(f'"AppState"\n{{\n\t"appid"\t\t"{target_id}"\n\t"name"\t\t"{base_name}"\n}}\n')

            cache_dir = os.path.join(tmpdir, "Library", "Application Support", "Steam", "appcache", "librarycache", target_id)
            os.makedirs(cache_dir, exist_ok=True)
            with open(os.path.join(cache_dir, "0123456789abcdef0123456789abcdef01234567.jpg"), "w") as f:
                f.write("mock")

        # Set up environment pointing to fake discord socket and hermetic HOME
        env = dict(os.environ)
        env["HOME"] = tmpdir
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
            if expected_art_substr:
                assert expected_art_substr in activity["assets"]["large_image"], f"Expected '{expected_art_substr}' in {activity['assets']['large_image']}"
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

        # Create hermetic mock Steam manifest
        steam_apps = os.path.join(tmpdir, "Library", "Application Support", "Steam", "steamapps")
        os.makedirs(steam_apps, exist_ok=True)
        manifest_path = os.path.join(steam_apps, "appmanifest_205100.acf")
        with open(manifest_path, "w") as f:
            f.write('"AppState"\n{\n\t"appid"\t\t"205100"\n\t"name"\t\t"Dishonored"\n}\n')

        env = dict(os.environ)
        env["HOME"] = tmpdir
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

def test_details_fallback():
    with tempfile.TemporaryDirectory(dir="/tmp") as tmpdir:
        sock_path = os.path.join(tmpdir, "discord-ipc-0")
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(sock_path)
        server.listen(1)

        steam_apps = os.path.join(tmpdir, "Library", "Application Support", "Steam", "steamapps")
        os.makedirs(steam_apps, exist_ok=True)
        with open(os.path.join(steam_apps, "appmanifest_205100.acf"), "w") as f:
            f.write('"AppState"\n{\n\t"appid"\t\t"205100"\n\t"name"\t\t"Dishonored"\n}\n')

        env = dict(os.environ)
        env["HOME"] = tmpdir
        env["DISCORD_RPC_SOCKET_DIR"] = tmpdir
        env["STEAM_COMPAT_APP_ID"] = "205100"
        env["DISCORD_RPC_MODE"] = "fallback"
        env["DYLD_INSERT_LIBRARIES"] = os.path.abspath("build/discord_bridge.dylib")

        fake_wine = os.path.join(tmpdir, "wine")
        subprocess.run(["cc", "-x", "c", "-o", fake_wine, "-"], input=b"#include <unistd.h>\nint main(){ sleep(10); return 0; }\n", check=True)
        proc = subprocess.Popen([fake_wine], env=env)
        try:
            server.settimeout(4.0)
            client, _ = server.accept()
            client.settimeout(4.0)

            op, length = struct.unpack("<II", client.recv(8))
            assert op == 0
            client.recv(length)
            ready = json.dumps({"cmd": "DISPATCH", "evt": "READY", "data": {}}).encode("utf-8")
            client.sendall(struct.pack("<II", 1, len(ready)) + ready)

            # First SET_ACTIVITY has name + status_display_type -> reject with ERROR
            op, length = struct.unpack("<II", client.recv(8))
            assert op == 1
            first = json.loads(client.recv(length).decode("utf-8"))
            assert first["args"]["activity"].get("name") == "Dishonored"
            err = json.dumps({"cmd": "SET_ACTIVITY", "evt": "ERROR", "data": {"code": 4000}}).encode("utf-8")
            client.sendall(struct.pack("<II", 1, len(err)) + err)

            # Retry should omit name & status_display_type and keep details
            op, length = struct.unpack("<II", client.recv(8))
            assert op == 1
            retry = json.loads(client.recv(length).decode("utf-8"))
            act = retry["args"]["activity"]
            assert "name" not in act, f"Expected name omitted on retry, got {act}"
            assert "status_display_type" not in act
            assert act["details"] == "Playing Dishonored"
            print("  ok    Discord details fallback verified when name override is rejected")
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except Exception:
                proc.kill()
            server.close()

def test_config_conf(config_text, extra_env=None, expected_state="__UNSET__", expected_art_substr="library_600x900.jpg"):
    with tempfile.TemporaryDirectory(dir="/tmp") as tmpdir:
        sock_path = os.path.join(tmpdir, "discord-ipc-0")
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(sock_path)
        server.listen(1)

        # Stage dylib, helper, and config.conf together in tmpdir
        plugin_dir = os.path.join(tmpdir, "plugin")
        os.makedirs(plugin_dir, exist_ok=True)
        dylib_copy = os.path.join(plugin_dir, "discord_bridge.dylib")
        shutil.copy2("build/discord_bridge.dylib", dylib_copy)
        shutil.copy2("build/notproton-discord.exe", os.path.join(plugin_dir, "notproton-discord.exe"))
        with open(os.path.join(plugin_dir, "config.conf"), "w") as f:
            f.write(config_text)

        steam_apps = os.path.join(tmpdir, "Library", "Application Support", "Steam", "steamapps")
        os.makedirs(steam_apps, exist_ok=True)
        with open(os.path.join(steam_apps, "appmanifest_205100.acf"), "w") as f:
            f.write('"AppState"\n{\n\t"appid"\t\t"205100"\n\t"name"\t\t"Dishonored"\n}\n')

        env = dict(os.environ)
        env["HOME"] = tmpdir
        env["NOTPROTON_DISCORD_SOCKET_DIR"] = tmpdir
        env["STEAM_COMPAT_APP_ID"] = "205100"
        env["DYLD_INSERT_LIBRARIES"] = dylib_copy
        if extra_env:
            env.update(extra_env)

        fake_wine = os.path.join(tmpdir, "wine")
        subprocess.run(["cc", "-x", "c", "-o", fake_wine, "-"], input=b"#include <unistd.h>\nint main(){ sleep(10); return 0; }\n", check=True)

        proc = subprocess.Popen([fake_wine], env=env)
        try:
            server.settimeout(3.0)
            client, _ = server.accept()
            client.settimeout(3.0)

            hdr = client.recv(8)
            op, length = struct.unpack("<II", hdr)
            assert op == 0
            client.recv(length)

            ready = json.dumps({"cmd": "DISPATCH", "evt": "READY", "data": {}}).encode("utf-8")
            client.sendall(struct.pack("<II", 1, len(ready)) + ready)

            hdr = client.recv(8)
            op, length = struct.unpack("<II", hdr)
            assert op == 1
            act_payload = json.loads(client.recv(length).decode("utf-8"))
            activity = act_payload["args"]["activity"]

            if expected_state is None:
                assert "state" not in activity, f"Expected state omitted, got {activity.get('state')}"
            elif expected_state != "__UNSET__":
                assert activity.get("state") == expected_state, f"Expected state '{expected_state}', got {activity.get('state')}"
            if expected_art_substr:
                assert expected_art_substr in activity["assets"]["large_image"], f"Expected '{expected_art_substr}' in {activity['assets']['large_image']}"
            print(f"  ok    config.conf verified (expected_state={expected_state}, art={expected_art_substr})")
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except Exception:
                proc.kill()
            server.close()

def test_external_library_and_shortcuts():
    # 1. External Steam library via libraryfolders.vdf
    with tempfile.TemporaryDirectory(dir="/tmp") as tmpdir:
        sock_path = os.path.join(tmpdir, "discord-ipc-0")
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(sock_path)
        server.listen(1)

        steam_root = os.path.join(tmpdir, "Library", "Application Support", "Steam")
        primary_apps = os.path.join(steam_root, "steamapps")
        ext_apps = os.path.join(tmpdir, "ExternalSSD", "SteamLibrary", "steamapps")
        os.makedirs(primary_apps, exist_ok=True)
        os.makedirs(ext_apps, exist_ok=True)
        with open(os.path.join(primary_apps, "libraryfolders.vdf"), "w") as f:
            f.write(f'"libraryfolders"\n{{\n\t"1"\n\t{{\n\t\t"path"\t\t"{os.path.join(tmpdir, "ExternalSSD", "SteamLibrary")}"\n\t}}\n}}\n')
        with open(os.path.join(ext_apps, "appmanifest_999111.acf"), "w") as f:
            f.write('"AppState"\n{\n\t"appid"\t\t"999111"\n\t"name"\t\t"External Drive Game"\n}\n')

        env = dict(os.environ)
        env["HOME"] = tmpdir
        env["DISCORD_RPC_SOCKET_DIR"] = tmpdir
        env["STEAM_COMPAT_APP_ID"] = "999111"
        env["DISCORD_RPC_MODE"] = "fallback"
        env["DISCORD_RPC_CLIENT_ID"] = "999888777666555444"
        env["DYLD_INSERT_LIBRARIES"] = os.path.abspath("build/discord_bridge.dylib")

        fake_wine = os.path.join(tmpdir, "wine")
        subprocess.run(["cc", "-x", "c", "-o", fake_wine, "-"], input=b"#include <unistd.h>\nint main(){ sleep(10); return 0; }\n", check=True)
        proc = subprocess.Popen([fake_wine], env=env)
        try:
            server.settimeout(4.0)
            client, _ = server.accept()
            client.settimeout(4.0)
            op, length = struct.unpack("<II", client.recv(8))
            assert op == 0
            hs = json.loads(client.recv(length).decode("utf-8"))
            assert hs["client_id"] == "999888777666555444", f"Expected custom client_id, got {hs}"
            ready = json.dumps({"cmd": "DISPATCH", "evt": "READY", "data": {}}).encode("utf-8")
            client.sendall(struct.pack("<II", 1, len(ready)) + ready)
            op, length = struct.unpack("<II", client.recv(8))
            assert op == 1
            act = json.loads(client.recv(length).decode("utf-8"))["args"]["activity"]
            assert act["name"] == "External Drive Game", f"Got {act}"
            print("  ok    libraryfolders.vdf external drive + custom client_id verified")
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except Exception:
                proc.kill()
            server.close()

    # 2. Non-Steam shortcut via binary userdata/*/config/shortcuts.vdf
    with tempfile.TemporaryDirectory(dir="/tmp") as tmpdir:
        sock_path = os.path.join(tmpdir, "discord-ipc-0")
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(sock_path)
        server.listen(1)

        cfg_dir = os.path.join(tmpdir, "Library", "Application Support", "Steam", "userdata", "12345", "config")
        os.makedirs(cfg_dir, exist_ok=True)
        shortcut_id = 0x12345678
        steam_game_id = str((shortcut_id << 32) | 0x02000000)
        # Binary VDF: \x00shortcuts\x00 \x000\x00 \x02appid\x00<4B> \x01AppName\x00Shortcut Game\x00 \x08 \x08 \x08
        vdf = (
            b"\x00shortcuts\x00"
            b"\x000\x00"
            b"\x02appid\x00" + struct.pack("<I", shortcut_id) +
            b"\x01AppName\x00Shortcut Game\x00"
            b"\x08\x08\x08"
        )
        with open(os.path.join(cfg_dir, "shortcuts.vdf"), "wb") as f:
            f.write(vdf)

        env = dict(os.environ)
        env["HOME"] = tmpdir
        env["DISCORD_RPC_SOCKET_DIR"] = tmpdir
        env["SteamGameId"] = steam_game_id
        env["DISCORD_RPC_MODE"] = "fallback"
        env["DYLD_INSERT_LIBRARIES"] = os.path.abspath("build/discord_bridge.dylib")

        fake_wine = os.path.join(tmpdir, "wine")
        subprocess.run(["cc", "-x", "c", "-o", fake_wine, "-"], input=b"#include <unistd.h>\nint main(){ sleep(10); return 0; }\n", check=True)
        proc = subprocess.Popen([fake_wine], env=env)
        try:
            server.settimeout(4.0)
            client, _ = server.accept()
            client.settimeout(4.0)
            op, length = struct.unpack("<II", client.recv(8))
            assert op == 0
            client.recv(length)
            ready = json.dumps({"cmd": "DISPATCH", "evt": "READY", "data": {}}).encode("utf-8")
            client.sendall(struct.pack("<II", 1, len(ready)) + ready)
            op, length = struct.unpack("<II", client.recv(8))
            assert op == 1
            act = json.loads(client.recv(length).decode("utf-8"))["args"]["activity"]
            assert act["name"] == "Shortcut Game", f"Expected 'Shortcut Game', got {act}"
            assert "assets" not in act, f"Expected no Steam assets for non-Steam shortcut, got {act}"
            print("  ok    shortcuts.vdf binary non-Steam shortcut title verified")
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except Exception:
                proc.kill()
            server.close()

def main():
    test_game("205100", "Dishonored")
    test_game("312520", "Rain World")
    test_game("632360", "Risk of Rain 2")
    # Test overrides
    test_game("205100", "Dishonored (Custom Mod)", extra_env={"NOTPROTON_DISCORD_TITLE": "Dishonored (Custom Mod)", "NOTPROTON_DISCORD_FORCE_AUTO": "1"})
    test_game("205100", "Dishonored", extra_env={"NOTPROTON_DISCORD_STATE": "Stealth Run", "NOTPROTON_DISCORD_FORCE_AUTO": "1"}, expected_state="Stealth Run")
    test_game(None, "Dishonored", extra_env={"NOTPROTON_DISCORD_APPID": "205100", "NOTPROTON_DISCORD_FORCE_AUTO": "1"})
    # Test art options
    test_game("205100", "Dishonored", extra_env={"NOTPROTON_DISCORD_ART": "header", "NOTPROTON_DISCORD_FORCE_AUTO": "1"}, expected_art_substr="header.jpg")
    test_game("205100", "Dishonored", extra_env={"NOTPROTON_DISCORD_ART": "logo", "NOTPROTON_DISCORD_FORCE_AUTO": "1"}, expected_art_substr="logo.png")
    test_game("205100", "Dishonored", extra_env={"NOTPROTON_DISCORD_ART": "icon", "NOTPROTON_DISCORD_FORCE_AUTO": "1"}, expected_art_substr="steamcommunity/public/images/apps/205100/")
    # Test new direct DISCORD_RPC_* names & install folder fallback
    test_game("205100", "Dishonored (Mod)", extra_env={"DISCORD_RPC_GAME_NAME": "Dishonored (Mod)", "DISCORD_RPC_MODE": "fallback", "DISCORD_RPC_SUBTITLE": "Co-op", "DISCORD_RPC_ARTWORK": "logo"}, expected_state="Co-op", expected_art_substr="logo.png")
    test_game(None, "My Non-Steam Game", extra_env={"STEAM_COMPAT_INSTALL_PATH": "/Users/test/Games/My Non-Steam Game", "DISCORD_RPC_MODE": "fallback"}, expected_art_substr=None)
    # Test details fallback on ERROR response
    test_details_fallback()
    # Test external libraryfolders.vdf, custom client_id, and binary shortcuts.vdf
    test_external_library_and_shortcuts()
    # Test config.conf defaults, empty subtitle omission, and env override
    test_config_conf('subtitle = "Gaming on Mac"\nartwork = header\nmode = fallback\n', expected_state="Gaming on Mac", expected_art_substr="header.jpg")
    test_config_conf('subtitle =\nmode = fallback\n', expected_state=None)
    test_config_conf('subtitle = Global Subtext\nmode = fallback\n', extra_env={"DISCORD_RPC_SUBTITLE": "Per-Game Override"}, expected_state="Per-Game Override")
    # Test disabled flags (both new and legacy)
    test_disabled({"DISCORD_RPC_DISABLE": "1"})
    test_disabled({"DISCORD_RPC_MODE": "native_only"})
    test_disabled({"NOTPROTON_DISCORD_DISABLE": "1"})
    test_disabled({"NOTPROTON_DISCORD_DISABLE_AUTO": "1"})
    print("==> Auto-Presence test suite passed successfully!")

if __name__ == "__main__":
    main()
