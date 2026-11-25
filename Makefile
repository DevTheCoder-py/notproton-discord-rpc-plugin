CC ?= clang
MINGW_CC ?= x86_64-w64-mingw32-gcc
CFLAGS ?= -std=c17 -O2 -Wall -Wextra -Werror
MACOS_ARCHS ?= -arch arm64 -arch x86_64 -mmacosx-version-min=12.0

SRC_DIR := src
BUILD_DIR := build
PLUGIN_DIR := plugin
TEST_DIR := tests

NOTPROTON_PLUGINS_DIR ?= $(HOME)/Library/Application Support/notproton/plugins
PLUGIN_TARGET_DIR := $(NOTPROTON_PLUGINS_DIR)/discord-rpc

DYLIB_TARGET := $(BUILD_DIR)/discord_bridge.dylib
HELPER_TARGET := $(BUILD_DIR)/notproton-discord.exe
HOST_TARGET := $(BUILD_DIR)/discord-host
TEST_CLIENT := $(BUILD_DIR)/test-client.exe

.PHONY: all clean check wine-check test install uninstall

all: $(DYLIB_TARGET) $(HELPER_TARGET) $(HOST_TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(DYLIB_TARGET): $(SRC_DIR)/bridge.c $(SRC_DIR)/protocol.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(MACOS_ARCHS) -shared -fPIC -I $(SRC_DIR) $< -o $@
	codesign -f -s - --identifier com.notproton.plugin.discord.bridge $@

$(HOST_TARGET): $(SRC_DIR)/bridge.c $(SRC_DIR)/protocol.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(MACOS_ARCHS) -DBUILD_HOST_CLI -I $(SRC_DIR) $< -o $@
	codesign -f -s - --identifier com.notproton.plugin.discord.host $@

$(HELPER_TARGET): $(SRC_DIR)/pipe.c $(SRC_DIR)/protocol.h | $(BUILD_DIR)
	$(MINGW_CC) $(CFLAGS) -mwindows -I $(SRC_DIR) $< -lws2_32 -o $@

$(TEST_CLIENT): $(TEST_DIR)/client.c $(SRC_DIR)/protocol.h | $(BUILD_DIR)
	$(MINGW_CC) $(CFLAGS) -I $(SRC_DIR) $< -o $@

check: all $(TEST_CLIENT)
	python3 $(TEST_DIR)/test_host.py $(HOST_TARGET)
	python3 $(TEST_DIR)/test_plugin.py
	python3 $(TEST_DIR)/test_autopresence.py

wine-check: all $(TEST_CLIENT)
	python3 $(TEST_DIR)/test_wine.py

test: check

install: all
	@mkdir -p "$(NOTPROTON_PLUGINS_DIR)"
	@mkdir -p "$(PLUGIN_TARGET_DIR)"
	cp -f "$(PLUGIN_DIR)/discord-rpc.conf" "$(NOTPROTON_PLUGINS_DIR)/discord-rpc.conf"
	cp -f "$(DYLIB_TARGET)" "$(PLUGIN_TARGET_DIR)/discord_bridge.dylib"
	cp -f "$(HELPER_TARGET)" "$(PLUGIN_TARGET_DIR)/notproton-discord.exe"
	@if [ ! -f "$(PLUGIN_TARGET_DIR)/config.conf" ] && [ -f "$(PLUGIN_DIR)/config.conf" ]; then cp "$(PLUGIN_DIR)/config.conf" "$(PLUGIN_TARGET_DIR)/config.conf"; fi
	@echo "==> Successfully installed Discord RPC Plugin to $(NOTPROTON_PLUGINS_DIR)"
	@echo "    - Plugin config: $(NOTPROTON_PLUGINS_DIR)/discord-rpc.conf"
	@echo "    - Plugin binaries: $(PLUGIN_TARGET_DIR)/"

uninstall:
	rm -f "$(NOTPROTON_PLUGINS_DIR)/discord-rpc.conf"
	rm -rf "$(PLUGIN_TARGET_DIR)"
	@echo "==> Successfully uninstalled Discord RPC Plugin."

clean:
	rm -rf $(BUILD_DIR)
