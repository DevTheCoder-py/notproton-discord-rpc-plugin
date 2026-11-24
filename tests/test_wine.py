#!/usr/bin/env python3
"""Run the bridge and Windows clients in a disposable Wine prefix."""
import concurrent.futures
import contextlib
import os
from pathlib import Path
import selectors
import socket
import subprocess
import sys
import tempfile
import threading
import time

build_dir = Path(__file__).resolve().parents[1] / "build"
host_bin = build_dir / "discord-host"
helper_bin = build_dir / "notproton-discord.exe"
client_bin = build_dir / "test-client.exe"

default_wine = Path("/Users/raihan/Library/Application Support/notproton/runners/crossover-26.3.0.39832/CrossOver/CrossOver-Hosted Application/wineloader")
default_wineserver = Path("/Users/raihan/Library/Application Support/notproton/runners/crossover-26.3.0.39832/CrossOver/CrossOver-Hosted Application/wineserver")

wine = os.environ.get("DISCORD_TEST_WINE") or (str(default_wine) if default_wine.exists() else None)
wineserver = os.environ.get("DISCORD_TEST_WINESERVER") or (str(default_wineserver) if default_wineserver.exists() else None)

if not wine or not wineserver:
    print("SKIP: Wine runtime executables not found (set DISCORD_TEST_WINE and DISCORD_TEST_WINESERVER)")
    sys.exit(0)

if not host_bin.exists() or not helper_bin.exists() or not client_bin.exists():
    print(f"SKIP: Required test binaries missing in {build_dir}")
    sys.exit(0)

print(f"Testing with Wine: {wine}")
with tempfile.TemporaryDirectory(prefix="np-wine-", dir="/tmp") as directory:
    tmp = Path(directory)
    env = {k: v for k, v in os.environ.items() if not k.startswith(("DYLD_", "DSB_", "NOTPROTON_"))}
    env.update(WINEPREFIX=str(tmp / "prefix"), WINESERVER=wineserver, WINEDEBUG="-all")
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(str(tmp / "discord-ipc-0"))
    server.listen()
    server.settimeout(0.1)
    closed = threading.Event()
    peers, workers = [], []

    def echo(peer):
        with peer:
            try:
                while data := peer.recv(1009):
                    peer.sendall(data)
            except OSError:
                pass

    def accept():
        while not closed.is_set():
            try:
                peer, _ = server.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            peers.append(peer)
            worker = threading.Thread(target=echo, args=(peer,))
            workers.append(worker)
            worker.start()

    thread = threading.Thread(target=accept)
    thread.start()
    host = None
    try:
        print("Initializing Wine prefix...")
        subprocess.run([wine, "wineboot", "-u"], env=env, check=True, timeout=90,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        host = subprocess.Popen(
            [str(host_bin), wine, str(helper_bin)],
            env=dict(env, NOTPROTON_DISCORD_SOCKET_DIR=str(tmp)),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        with selectors.DefaultSelector() as selector:
            selector.register(host.stdout, selectors.EVENT_READ)
            assert selector.select(15), "Wine bridge readiness timed out"
        ready = host.stdout.readline()
        assert ready == b"ready\n", f"unexpected readiness output: {ready!r}"

        def client(seed, executable=client_bin):
            result = subprocess.run([wine, str(executable), "echo", "1048576", str(seed)],
                                    env=env, capture_output=True, text=True, timeout=30)
            assert result.returncode == 0, (result.stdout, result.stderr)
            print(result.stdout.rstrip())

        print("Testing Windows pipe client communication...")
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(client, range(4)))

        print("  ok    Windows named pipe round-trip transfer passed")
    finally:
        if host and host.poll() is None:
            host.terminate()
            host.wait(timeout=8)
        subprocess.run([wineserver, "-k"], env=env, timeout=10)
        subprocess.run([wineserver, "-w"], env=env, timeout=10)
        closed.set()
        server.close()
        thread.join(timeout=2)
        for peer in peers:
            with contextlib.suppress(OSError):
                peer.shutdown(socket.SHUT_RDWR)
        for worker in workers:
            worker.join(timeout=2)

print("==> Discord Wine integration check passed!")
