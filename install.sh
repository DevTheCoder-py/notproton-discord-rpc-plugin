#!/usr/bin/env bash
set -euo pipefail

DEST_DIR="$HOME/Library/Application Support/notproton/plugins"
PLUGIN_SUBDIR="$DEST_DIR/discord-rpc"
CONF_FILE="$DEST_DIR/discord-rpc.conf"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ "${1:-}" = "--uninstall" ] || [ "${1:-}" = "-u" ]; then
    echo "==> Uninstalling NotProton Discord Rich Presence Plugin..."
    rm -f "$CONF_FILE"
    rm -rf "$PLUGIN_SUBDIR"
    echo "==> Uninstallation complete."
    exit 0
fi

# Check for pre-built binaries or build from source
if [ ! -f "$SCRIPT_DIR/build/discord_bridge.dylib" ] || [ ! -f "$SCRIPT_DIR/build/notproton-discord.exe" ]; then
    echo "==> Checking build dependencies..."
    if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
        echo "Error: x86_64-w64-mingw32-gcc is required to build the Windows pipe helper."
        echo "Please install it via Homebrew:"
        echo "    brew install mingw-w64"
        exit 1
    fi

    echo "==> Building NotProton Discord Rich Presence Plugin..."
    make -C "$SCRIPT_DIR" all
fi

echo "==> Installing to $DEST_DIR..."
mkdir -p "$PLUGIN_SUBDIR"
cp -f "$SCRIPT_DIR/plugin/discord-rpc.conf" "$CONF_FILE"
cp -f "$SCRIPT_DIR/build/discord_bridge.dylib" "$PLUGIN_SUBDIR/discord_bridge.dylib"
cp -f "$SCRIPT_DIR/build/notproton-discord.exe" "$PLUGIN_SUBDIR/notproton-discord.exe"
if [ ! -f "$PLUGIN_SUBDIR/config.conf" ] && [ -f "$SCRIPT_DIR/plugin/config.conf" ]; then
    cp "$SCRIPT_DIR/plugin/config.conf" "$PLUGIN_SUBDIR/config.conf"
fi

echo ""
echo "======================================================================"
echo "  NotProton Discord Rich Presence Plugin Installed Successfully!"
echo "======================================================================"
echo "Installed components:"
echo "  - Plugin Config: $CONF_FILE"
echo "  - macOS Bridge:  $PLUGIN_SUBDIR/discord_bridge.dylib"
echo "  - Windows Pipe:  $PLUGIN_SUBDIR/notproton-discord.exe"
echo ""
echo "How to use:"
echo "  1. In the NotProton app, click 'Plugins' to verify the plugin is active."
echo "  2. In Steam, right click any game -> Properties -> Compatibility / General."
echo "  3. In the Developer options, turn on 'Discord Rich Presence'."
echo "  4. Launch the game with Discord running on macOS. Your Rich Presence"
echo "     will now automatically display in Discord!"
echo ""
echo "To uninstall at any time:"
echo "  ./install.sh --uninstall"
echo "  (or: rm -rf '$DEST_DIR/discord-rpc*')"
echo "======================================================================"

# Optional interactive cleanup of the cloned repository
if [ -t 0 ] && [ "${1:-}" != "--no-prompt" ]; then
    echo ""
    read -r -p "Would you like to delete this cloned build directory now? [y/N]: " response
    case "$response" in
        [yY][eE][sS]|[yY])
            # Strict safety checks before deletion
            if [ -n "$SCRIPT_DIR" ] && \
               [ "$SCRIPT_DIR" != "/" ] && \
               [ "$SCRIPT_DIR" != "$HOME" ] && \
               [ -d "$SCRIPT_DIR/.git" ] && \
               [ -f "$SCRIPT_DIR/install.sh" ]; then
                echo "==> Cleaning up build directory ($SCRIPT_DIR)..."
                cd "$HOME"
                rm -rf "$SCRIPT_DIR"
                echo "==> Cleaned up. Installation remains active in $DEST_DIR."
            fi
            ;;
        *)
            ;;
    esac
fi
