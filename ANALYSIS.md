# Architecture Notes: Discord IPC under NotProton

## Transport Gap

Windows Discord RPC clients communicate with the Discord desktop client over named pipes (`\\.\pipe\discord-ipc-0` through `9`). On macOS, Discord listens on a UNIX domain socket (`$TMPDIR/discord-ipc-0` or `/var/folders/.../T/discord-ipc-0`). The wire framing on both operating systems is identical: a 4-byte little-endian opcode, a 4-byte little-endian length prefix, and an uncompressed JSON payload.

Wine implements Windows named pipes internally within `wineserver`. Wine does not bridge Win32 named pipes to host UNIX domain sockets. When a game running under Wine calls `CreateFileA` on `\\.\pipe\discord-ipc-0`, wineserver looks for an existing pipe object created by a Wine process. Because nothing in the prefix has bound that name, the call fails with `ERROR_FILE_NOT_FOUND` (2).

## Earlier Integration Pattern

The original implementation (`sn0wst0rm/NotProton@b358d97`) addressed this by introducing two separate components:

1. A Win32 helper (`notproton-discord.exe`) running inside the Wine prefix that calls `CreateNamedPipeA` to bind `\\.\pipe\discord-ipc-0` and relays traffic over a loopback TCP socket (`127.0.0.1:<port>`).
2. A native Mach-O host binary (`host`) that listens on the loopback TCP port, bridges data to the macOS Discord UNIX socket, and manages token-based authentication.

In NotProton 1.1.5, loading this bridge required patching NotProton's core launcher scripts (`compat_run.sh`), hardcoding a React/JSX toggle into `webpatch.c` for Steam's UI, and bundling the helper binaries inside the NotProton app bundle. Because NotProton lacked a modular extension mechanism at the time, any updates to the bridge required maintaining a separate fork of NotProton.

## Plugin Implementation

NotProton 1.2+ introduced a declarative plugin loader. Dropping a configuration file into `~/Library/Application Support/notproton/plugins/` exposes a toggle in Steam's Developer settings panel and injects a shared library into the target runner via `DYLD_INSERT_LIBRARIES`.

This plugin replaces the standalone host binary and manual shell scripts with a single dylib (`discord_bridge.dylib`) and the Win32 helper (`notproton-discord.exe`):

1. Steam executes NotProton's compatibility runner with `discord_bridge.dylib` added to `DYLD_INSERT_LIBRARIES`.
2. The dylib constructor runs in the macOS Wine loader's address space. It validates that the parent process is Wine, binds an ephemeral loopback TCP port on `127.0.0.1`, generates a 64-byte random authentication token, and spawns `notproton-discord.exe` inside the prefix via `posix_spawn`.
3. `notproton-discord.exe` binds 10 named pipe instances (`\\.\pipe\discord-ipc-0` through `9`). When a game connects, the helper authenticates with the loopback listener, and the host dylib relays framed packets to Discord's UNIX domain socket.
4. When Wine shuts down, the dylib destructor signals the helper process, closes active socket connections, and reaps the child process.

## Process Detection vs. IPC Presence

Discord on macOS maintains a database of known games and scans running applications using `NSWorkspace` and `proc_pidinfo`. In standalone Wine or CrossOver setups, foreground bottles expose their window hierarchy and process identifiers directly to the WindowServer, which occasionally allows Discord to match game titles based on executable name or window text.

Under NotProton, Steam games run through a generated app bundle where `Info.plist` sets `<key>LSUIElement</key><true/>` and names the executable `wine`. This is done deliberately to prevent Wine helper processes from appearing in the macOS Dock. However, Discord's process scanner explicitly filters out `LSUIElement` background daemons, and `wine` is not registered as a known game executable in Discord's database. Games running under NotProton are therefore invisible to Discord's local scanner.

Furthermore, games that do not implement the Discord RPC SDK (such as older titles like *Dishonored* or games where Discord libraries are only utilized for authentication like *Risk of Rain 2*) never open named pipes.

## Fallback Presence

To handle games without native Discord RPC support, `discord_bridge.dylib` includes a fallback presence thread:

1. At startup, the bridge extracts the game's Steam AppID from `STEAM_COMPAT_APP_ID`, `SteamAppId`, or the launcher path.
2. It parses the corresponding Steam manifest (`appmanifest_<appid>.acf`) to obtain the canonical game title.
3. The thread waits for a configurable grace period (default 3 seconds) for the game to write to any of the Win32 named pipes. If native named pipe traffic is detected, the fallback thread terminates immediately and named pipe forwarding takes precedence.
4. If the grace period expires with no pipe traffic, the bridge connects directly to Discord's UNIX socket under application ID `1508914471433928824`.
5. By default, it fetches the official 600x900 HD vertical box art from Steam's CDN (`shared.fastly.steamstatic.com/store_item_assets/steam/apps/<appid>/library_600x900.jpg`), or other asset formats (`header`, `hero`, `logo`, `icon`) configured via `NOTPROTON_DISCORD_ART`.
6. It sends a `SET_ACTIVITY` payload with `status_display_type: 2`, the game title, elapsed start time, and image assets.
7. If the game later initiates named pipe traffic, the bridge sends a clear-activity frame and hands control over to the game's native RPC session.
