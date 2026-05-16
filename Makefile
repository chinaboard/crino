# crino — ESP32-C3/C6 firmware template (WiFi/OTA/Web UI chassis)
#
# Build runs inside espressif/idf:v6.0.1 (Docker), since macOS Docker can't
# pass through USB. Flash/monitor run on the host with espflash, which talks
# to /dev/cu.usbmodem101 directly.
#
# Pick a board with BOARD=<name>; the file boards/<name>.mk supplies all
# board-specific defaults (TARGET, LED_KIND, LED_GPIO…). Each board lives in
# its own per-target build directory (build-esp32cN/) so you can keep both
# binaries around and switch with no fullclean dance.
#
# Built-in boards:
#   BOARD=supermini-c6   ESP32-C6 SuperMini, WS2812 on GPIO8 (default)
#   BOARD=supermini-c3   ESP32-C3 SuperMini, plain blue LED on GPIO8
#   BOARD=generic        no LED, c6 target — override TARGET=... yourself
# Add a new board by dropping a new boards/<name>.mk; no Makefile edit needed.
#
# CLI overrides still work:  make build BOARD=supermini-c3 LED_BRIGHTNESS=30

PORT       ?= /dev/cu.usbmodem101
BAUD       ?= 921600
IDF_IMAGE  ?= espressif/idf:v6.0.1
PROJECT    ?= crino
BOARD      ?= supermini-c6

# Board overlay sets TARGET / LED_* defaults. Use ?= inside so explicit CLI
# values (which Make sets before this include runs) win.
include boards/$(BOARD).mk

# Allow TARGET to be either short form (`c3`, `c6`) or the canonical
# IDF identifier (`esp32c3`, `esp32c6`). Strip the leading "esp32"
# if present, then prefix it once — avoids the `esp32esp32c3` build
# dir if someone follows IDF convention and sets `TARGET=esp32c3`.
TARGET_BARE := $(patsubst esp32%,%,$(TARGET))
IDF_TARGET  := esp32$(TARGET_BARE)
BUILD_DIR   := build-$(IDF_TARGET)
# Per-target sdkconfig (the generated one with all expanded settings).
# Defaults overlay still comes from sdkconfig.defaults + sdkconfig.defaults.$(IDF_TARGET).
SDKCONFIG  := sdkconfig.$(IDF_TARGET)

# Build-time timezone (POSIX TZ string). Override per region:
#   make build TZ=JST-9        # Japan
#   make build TZ=EST5EDT      # US East
TZ         ?= CST-8

# IDF picks up EXTRA_CFLAGS from the env at build time. Inner values use
# escaped double quotes so the whole string can be wrapped in shell double
# quotes when passed to `docker run -e` below — single-quoting inside
# single-quoting doesn't compose.
EXTRA_CFLAGS := -DCR_TZ=\"$(TZ)\" \
                -DCR_LED_ENABLE=$(LED_ENABLE) \
                -DCR_LED_KIND=$(LED_KIND) \
                -DCR_LED_GPIO=$(LED_GPIO) \
                -DCR_LED_BRIGHTNESS=$(LED_BRIGHTNESS) \
                -DCR_BOOT_BUTTON_GPIO=$(BOOT_BUTTON_GPIO)

# Branding override: bake a different prefix into the chassis-derived
# identifiers (mDNS hostname, SoftAP SSIDs, /api/system/status mdns_host).
# Default in cr_util.h is "crino". Keep short — "<prefix>-setup-XXXX"
# must fit in the 32-char WiFi SSID limit.
#   make build BOARD=supermini-c3 HOSTNAME_PREFIX=fooboard
ifdef HOSTNAME_PREFIX
EXTRA_CFLAGS += -DCR_HOSTNAME_PREFIX=\"$(HOSTNAME_PREFIX)\"
endif

# Debug-only WiFi cred override: skip the SoftAP setup wizard and boot a
# blank board straight into STA mode using these creds. Never set in CI.
#   make build DEBUG_WIFI_SSID=IoToI DEBUG_WIFI_PASS=54383845
ifdef DEBUG_WIFI_SSID
EXTRA_CFLAGS += -DDEBUG_WIFI_SSID=\"$(DEBUG_WIFI_SSID)\"
endif
ifdef DEBUG_WIFI_PASS
EXTRA_CFLAGS += -DDEBUG_WIFI_PASS=\"$(DEBUG_WIFI_PASS)\"
endif

DOCKER_RUN = docker run --rm -v $(PWD):/project -w /project $(EXTRA_DOCKER_VOLUMES) \
             -e IDF_TARGET=$(IDF_TARGET) -e EXTRA_CFLAGS="$(EXTRA_CFLAGS)" \
             -e EXTRA_COMPONENT_DIRS="$(EXTRA_COMPONENT_DIRS)" $(IDF_IMAGE)
DOCKER_TTY = docker run --rm -it -v $(PWD):/project -w /project $(EXTRA_DOCKER_VOLUMES) \
             -e IDF_TARGET=$(IDF_TARGET) -e EXTRA_CFLAGS="$(EXTRA_CFLAGS)" \
             -e EXTRA_COMPONENT_DIRS="$(EXTRA_COMPONENT_DIRS)" $(IDF_IMAGE)

# EXTRA_COMPONENT_DIRS lets downstream apps live next to the chassis
# without polluting components/. Pass it through to cmake explicitly —
# always, even when empty, so toggling the smoketest off doesn't leave
# the previous value sitting in CMakeCache.txt:
#   make build BOARD=supermini-c3 EXTRA_COMPONENT_DIRS=tests   # ON
#   make build BOARD=supermini-c3                              # OFF (was sticky)
EXTRA_CMAKE := -DEXTRA_COMPONENT_DIRS=$(EXTRA_COMPONENT_DIRS)

# Downstream apps that need a different partition layout (BLE-heavy apps need
# bigger OTA slots, sensor apps may want more storage, etc.) can either:
#
#   1. Pick a chassis-provided preset:
#        make build PARTITION_PRESET=ota-only   # 1792K x2 OTA, no factory
#        make build PARTITION_PRESET=factory    # default: 1152K factory + 1216K x2 OTA
#
#   2. Hand-roll their own and pass an absolute path:
#        make build EXTRA_PARTITION_TABLE=/app/partitions.app.csv
#
# Implementation: we generate a tiny sdkconfig overlay file at build time
# that flips CONFIG_PARTITION_TABLE_CUSTOM_FILENAME to point at the chosen
# csv. -DCONFIG_FOO=bar on the cmake command line does NOT actually override
# kconfig values past the first reconfigure, so the sdkconfig-overlay route
# is the only stable way to switch partition tables from outside.
#
# The overlay file lives in the chassis tree (so the IDF container can read
# it from /project) and gets rebuilt every invocation (cheap — 2 lines).
PARTITION_PRESET ?= factory
ifeq ($(PARTITION_PRESET),factory)
  CHASSIS_PARTITION_TABLE := partitions.csv
else
  CHASSIS_PARTITION_TABLE := partitions.$(PARTITION_PRESET).csv
endif

# Final partition table: EXTRA_PARTITION_TABLE wins if set, else use preset.
ifdef EXTRA_PARTITION_TABLE
PARTITION_TABLE_PATH := $(EXTRA_PARTITION_TABLE)
else
PARTITION_TABLE_PATH := $(CHASSIS_PARTITION_TABLE)
endif

# Generate a tiny sdkconfig overlay every reconfigure so the partition table
# selection actually takes effect. We tack it onto the SDKCONFIG_DEFAULTS
# semicolon list LAST so it wins over any caller-supplied overlay too.
PARTITION_OVERLAY := sdkconfig.defaults.partition.gen
$(shell printf 'CONFIG_PARTITION_TABLE_CUSTOM=y\nCONFIG_PARTITION_TABLE_CUSTOM_FILENAME="$(PARTITION_TABLE_PATH)"\nCONFIG_PARTITION_TABLE_FILENAME="$(PARTITION_TABLE_PATH)"\n' > $(PARTITION_OVERLAY))

# Build the SDKCONFIG_DEFAULTS list:
#   sdkconfig.defaults                              (chassis baseline, always)
#   $(EXTRA_SDKCONFIG_DEFAULTS)                     (downstream overlay, optional)
#   $(PARTITION_OVERLAY)                            (partition selection, always last)
SDKCONFIG_DEFAULTS_LIST := sdkconfig.defaults
ifdef EXTRA_SDKCONFIG_DEFAULTS
SDKCONFIG_DEFAULTS_LIST := $(SDKCONFIG_DEFAULTS_LIST);$(EXTRA_SDKCONFIG_DEFAULTS)
endif
SDKCONFIG_DEFAULTS_LIST := $(SDKCONFIG_DEFAULTS_LIST);$(PARTITION_OVERLAY)
EXTRA_CMAKE += -DSDKCONFIG_DEFAULTS="$(SDKCONFIG_DEFAULTS_LIST)"

.PHONY: build flash monitor flash-monitor erase clean fullclean menuconfig size shell boards

boards:
	@echo "Available boards (set BOARD=<name>):"
	@for f in boards/*.mk; do \
		name=$$(basename $$f .mk); \
		first=$$(head -1 $$f | sed 's|^# *||'); \
		printf "  %-18s %s\n" "$$name" "$$first"; \
	done

build:
	$(DOCKER_RUN) idf.py -B $(BUILD_DIR) -DSDKCONFIG=$(SDKCONFIG) $(EXTRA_CMAKE) reconfigure build

flash:
	espflash flash \
		--port $(PORT) --baud $(BAUD) \
		--partition-table $(BUILD_DIR)/partition_table/partition-table.bin \
		--bootloader $(BUILD_DIR)/bootloader/bootloader.bin \
		$(BUILD_DIR)/$(PROJECT).elf

monitor:
	espflash monitor --port $(PORT)

flash-monitor: flash monitor

erase:
	espflash erase-flash --port $(PORT)

size:
	$(DOCKER_RUN) idf.py -B $(BUILD_DIR) -DSDKCONFIG=$(SDKCONFIG) size

menuconfig:
	$(DOCKER_TTY) idf.py -B $(BUILD_DIR) -DSDKCONFIG=$(SDKCONFIG) menuconfig

shell:
	$(DOCKER_TTY) bash

clean:
	$(DOCKER_RUN) idf.py -B $(BUILD_DIR) -DSDKCONFIG=$(SDKCONFIG) fullclean

# Wipe ALL per-target build dirs + generated sdkconfigs + managed components.
# Careful: only delete sdkconfig.esp32cN (generated), NOT sdkconfig.defaults*
# (checked into git as the source of truth).
fullclean:
	rm -rf build build-* sdkconfig sdkconfig.esp32* managed_components dependencies.lock
