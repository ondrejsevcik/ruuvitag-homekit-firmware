# Sources and flags of the native HomeKit variant.
# Included by targets/*/armgcc/Makefile when HOMEKIT=1.

HOMEKIT_DIR := $(PROJ_DIR)/homekit
ADK_DIR     := $(PROJ_DIR)/../external/HomeKitADK

# HomeKit ADK core. IP transport files are excluded, the rest is pruned by --gc-sections.
ADK_HAP_SOURCES := $(filter-out $(ADK_DIR)/HAP/HAPIP%.c $(ADK_DIR)/HAP/HAPJSONUtils.c, \
                     $(wildcard $(ADK_DIR)/HAP/*.c))

ADK_PAL_SOURCES := \
  $(ADK_DIR)/PAL/HAPAssert.c \
  $(ADK_DIR)/PAL/HAPLog.c \
  $(wildcard $(ADK_DIR)/PAL/HAPBase+*.c)

HOMEKIT_SOURCES := \
  $(ADK_HAP_SOURCES) \
  $(ADK_PAL_SOURCES) \
  $(HOMEKIT_DIR)/app_homekit.c \
  $(HOMEKIT_DIR)/homekit_db.c \
  $(HOMEKIT_DIR)/crypto/HAPOberon.c \
  $(HOMEKIT_DIR)/pal/HAPPlatform.c \
  $(HOMEKIT_DIR)/pal/HAPPlatformBLEPeripheralManager.c \
  $(HOMEKIT_DIR)/pal/HAPPlatformKeyValueStore.c \
  $(HOMEKIT_DIR)/pal/HAPPlatformTimer.c

# Setup salt / verifier, generated from homekit-setup.json (see tools/homekit_provision.py).
HOMEKIT_SETUP_DIR    := $(OUTPUT_DIRECTORY)/homekit_setup
HOMEKIT_SETUP_HEADER := $(HOMEKIT_SETUP_DIR)/homekit_setup_info.h
HOMEKIT_SETUP_STATE  ?= $(abspath $(PROJ_DIR)/../homekit-setup.json)

HOMEKIT_INCLUDES := \
  $(PROJ_DIR) \
  $(HOMEKIT_DIR) \
  $(HOMEKIT_DIR)/pal \
  $(HOMEKIT_SETUP_DIR) \
  $(ADK_DIR)/HAP \
  $(ADK_DIR)/PAL \
  $(ADK_DIR)/External/HTTP \
  $(ADK_DIR)/External/JSON \
  $(ADK_DIR)/External/Base64 \
  $(SDK_ROOT)/external/nrf_oberon/include

# HAP_LOG_LEVEL: 0 none, 1 errors, 2 info, 3 debug. Logs go to RTT in debug builds.
HAP_LOG_LEVEL ?= 0

HOMEKIT_CFLAGS := \
  -DHAP_IP=0 \
  -DHAP_LOG_LEVEL=$(HAP_LOG_LEVEL) \
  -DHAVE_CUSTOM_SINGLE_SHOT_CHACHA20_POLY1305=1 \
  -DHAP_DISABLE_ASSERTS=1 \
  -Wno-unused-local-typedefs

$(HOMEKIT_SETUP_HEADER): $(PROJ_DIR)/../tools/homekit_provision.py $(wildcard $(HOMEKIT_SETUP_STATE))
	python3 $< header $@ --state $(HOMEKIT_SETUP_STATE)

.PHONY: homekit_setup_header
homekit_setup_header: $(HOMEKIT_SETUP_HEADER)
