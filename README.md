# notproton-discord-rpc-plugin

A plugin for [NotProton](https://github.com/NotProtonNot/NotProton) (v1.2+) that bridges Discord game presence from Wine to the macOS Discord desktop client.

## Overview

```text
Wine Prefix (Windows)
  Game Process
      │
      │ (\\.\pipe\discord-ipc-0..9)
      ▼
  notproton-discord.exe
      │
      │ Loopback TCP (127.0.0.1)
      ▼
macOS Host
  discord_bridge.dylib
      │
      │ UNIX Domain Socket ($TMPDIR/discord-ipc-0)
      ▼
  Discord for Mac
```

Windows games communicate with Discord over Win32 named pipes (`\\.\pipe\discord-ipc-0` through `9`), whereas Discord on macOS listens on a UNIX domain socket. Wine does not bridge these sockets by default.

This plugin handles game presence through two methods:

- **Named Pipe Forwarding**: Injected into the Wine runner via `DYLD_INSERT_LIBRARIES`, `discord_bridge.dylib` spawns a Win32 helper (`notproton-discord.exe`) inside the prefix. The helper creates pipes `0` through `9` and relays IPC packets over an authenticated loopback TCP connection to the host dylib, which forwards them to Discord's local UNIX socket. Games with built-in Discord RPC SDK support or community mods communicate through this path.
- **Steam AppID Fallback**: For games that do not implement the Discord RPC SDK, the dylib reads `STEAM_COMPAT_APP_ID` and parses the local Steam app manifest (`appmanifest_<appid>.acf`) to retrieve the game title. If no named pipe connection occurs within 3 seconds, it connects directly to Discord and sets the presence using the resolved title, elapsed runtime, and the game's official 1:1 square icon from Steam's CDN (with store header fallback). If the game later opens a named pipe, this fallback status disconnects automatically.

### Technical Note: Process Detection vs. Fallback Presence

Discord for Mac tracks user activity through two separate mechanisms:

1. **Native Process Detection (`DiscordGameUtils`)**: Discord polls the macOS process table (`proc_pidinfo`, `NSWorkspace`) against its global game database (`games.json`). In standalone Wine or CrossOver, games running in foreground bottles may match on executable name or window title. Under NotProton, compatibility runners explicitly set `<key>LSUIElement</key><true/>` in the launcher `Info.plist` to prevent auxiliary Wine helper processes from cluttering the macOS Dock, and the executable binary is named `wine`. Discord filters out background `LSUIElement` agents, preventing automatic process-level detection.
2. **Native Named Pipe RPC**: Games programmed with the Discord SDK write directly to `\\.\pipe\discord-ipc-0`. The plugin proxies these packets verbatim to Discord's UNIX socket, delivering full real-time game telemetry (in-game state, party size, match scores).
3. **Steam AppID Fallback**: For games that lack native Discord RPC (or where Discord's process scanner fails to match the `wine` process), the plugin reads the Steam AppID and broadcasts a Rich Presence activity frame over Discord's IPC socket (`status_display_type: 2`) using registered Client ID `1508914471433928824`. This displays the game name, elapsed playtime timer, and official Steam square game icon. It is an IPC-level status broadcast rather than a macOS kernel process detection event.

## Requirements

- macOS 12.0+
- NotProton v1.2.0+
- Discord for Mac
- `clang` and `mingw-w64` (only required when building from source)

## Installation

### From Source (Quick Install)

Clone to your home directory and run the installer:

```bash
cd ~ && git clone https://github.com/DevTheCoder-py/notproton-discord-rpc-plugin.git && cd notproton-discord-rpc-plugin && ./install.sh
```

All plugin files are copied directly into `~/Library/Application Support/notproton/plugins/`. Once the installer finishes, it will offer to delete the cloned build folder for you (or you can safely delete it manually).

### Pre-built Binaries

1. Download `notproton-discord-rpc-plugin.zip` from Releases.
2. Extract the archive and copy the files to your NotProton plugins directory:

```text
~/Library/Application Support/notproton/plugins/
  ├── discord-rpc.conf
  └── discord-rpc/
      ├── discord_bridge.dylib
      └── notproton-discord.exe
```

If macOS Gatekeeper flags pre-built binaries downloaded via browser, remove the quarantine attribute (no sudo required):

```bash
xattr -dr com.apple.quarantine ~/Library/Application\ Support/notproton/plugins/discord-rpc/
```

## Configuration

1. In NotProton, verify that `Discord Rich Presence` appears under **Plugins**.
2. In Steam, open game **Properties** $\rightarrow$ **Compatibility** / **General**.
3. Under **Developer**, enable **Discord Rich Presence**.

### Environment Variables (Launch Options)

You can customize the plugin per-game by setting environment variables in Steam's **Launch Options** (`Properties` $\rightarrow$ `General` $\rightarrow$ `Launch Options`):

| Variable | Default | Description |
|---|---|---|
| `NOTPROTON_DISCORD_DISABLE=1` | `0` | Completely disables the Discord plugin for this game launch. |
| `NOTPROTON_DISCORD_DISABLE_AUTO=1` | `0` | Disables Steam AppID fallback presence (only relays native Discord SDK named pipes). |
| `NOTPROTON_DISCORD_FORCE_AUTO=1` | `0` | Skips the Win32 pipe helper and activates Steam presence immediately without the 3-second wait. |
| `NOTPROTON_DISCORD_TITLE="..."` | Auto-detected | Overrides the game title displayed in Discord (useful for mods, fan translations, or non-Steam shortcuts). |
| `NOTPROTON_DISCORD_STATE="..."` | `"Steam via NotProton"` | Overrides the activity status line subtitle. |
| `NOTPROTON_DISCORD_APPID=N` | Auto-detected | Manually sets the Steam AppID for icon and title resolution (useful for standalone shortcuts). |
| `NOTPROTON_DISCORD_WAIT_SEC=N` | `3` | Seconds to wait for native game pipe traffic before falling back to Steam AppID presence (0–60). |
| `NOTPROTON_DISCORD_SOCKET_DIR` | Auto-detected | Custom directory path for `discord-ipc-0` (overrides default Darwin temp paths). |

#### Examples

**Force instant presence with a custom subtitle:**
```bash
NOTPROTON_DISCORD_FORCE_AUTO=1 NOTPROTON_DISCORD_STATE="Singleplayer Campaign" %command%
```

**Override title for a mod or non-Steam shortcut:**
```bash
NOTPROTON_DISCORD_TITLE="Skyrim Together" NOTPROTON_DISCORD_APPID=489830 %command%
```

**Completely disable Discord status for a specific game:**
```bash
NOTPROTON_DISCORD_DISABLE=1 %command%
```

## Uninstallation

From the cloned repository:

```bash
./install.sh --uninstall
```

Or if you deleted the repository, remove the plugin files directly:

```bash
rm -rf ~/Library/Application\ Support/notproton/plugins/discord-rpc*
```

---

## Contributing

I don't really care and this probably won't break anytime soon. But if something actually breaks or Discord/Wine changes down the line, feel free to open a PR and I'll look at it.

---

## Acknowledgements

- Based on initial named-pipe bridge research and prototypes by [sn0wst0rm](https://github.com/sn0wst0rm) ([`sn0wst0rm/NotProton` (feat/discord-rich-presence)](https://github.com/sn0wst0rm/NotProton/tree/feat/discord-rich-presence)).
- Built for the [NotProton](https://github.com/NotProtonNot/NotProton) ecosystem.

---

## Disclaimer

Code implementation and debugging were heavily AI-assisted, while all project documentation, specifications, and architecture design were written and maintained by me.
