# Host-side tests for the HomeKit port.
#
# The tests are built for 32-bit ARM Linux (armhf) so that they can link the
# very same prebuilt nrf_oberon crypto library that runs on the RuuviTag.
# A Raspberry Pi (or any AArch64 machine with 32-bit userland support) runs
# them natively; elsewhere use qemu-arm.
#
# Requirements (Debian/Raspberry Pi OS):
#   apt install gcc-arm-linux-gnueabihf libc6:armhf gcc-arm-none-eabi
#
# Usage:
#   make -C tests/host        # build and run all tests

ROOT        := $(abspath ../..)
SDK_ROOT    ?= $(ROOT)/nRF5_SDK_15.3.0_59ac345
ADK         := $(ROOT)/external/HomeKitADK
HK          := $(ROOT)/src/homekit
BUILD       := _build
.DEFAULT_GOAL := all

CC          := arm-linux-gnueabihf-gcc
AR          := arm-linux-gnueabihf-ar
OBJCOPY     := arm-none-eabi-objcopy
EMBED_AR    := arm-none-eabi-ar
RUN         ?=

OBERON_LIB  := $(SDK_ROOT)/external/nrf_oberon/lib/cortex-m4/hard-float/liboberon_2.0.7.a
HOST_OBERON := $(BUILD)/liboberon_host.a

CFLAGS := -mcpu=cortex-a7 -mthumb -mfpu=vfpv4 -mfloat-abi=hard \
          -std=gnu11 -O1 -g -Wall -Wextra -Werror \
          -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers \
          -DHAP_IP=0 -DHAP_LOG_LEVEL=1 -DHAP_TESTING=1 \
          -DHAVE_CUSTOM_SINGLE_SHOT_CHACHA20_POLY1305=1 \
          -I$(ADK)/HAP -I$(ADK)/PAL -I$(ADK)/PAL/Linux -I$(ADK)/External/HTTP \
          -I$(SDK_ROOT)/external/nrf_oberon/include -I$(HK) -I$(HK)/pal

ADK_BASE := $(ADK)/PAL/HAPAssert.c $(ADK)/PAL/HAPLog.c $(wildcard $(ADK)/PAL/HAPBase+*.c) \
            $(ADK)/PAL/Linux/HAPPlatformAbort.c $(ADK)/PAL/Linux/HAPPlatformLog.c

TESTS := crypto_test setup_info_test

# Compile and link, hiding only the harmless "variable-size enums" linker note caused by
# linking the M-profile nrf_oberon objects. Compiler errors still fail the build.
quiet_enums = 2>$@.log; status=$$?; grep -v "variable-size enums" $@.log >&2; rm -f $@.log; exit $$status

# HomeKit accessory simulator: firmware HomeKit code + simulated SoftDevice / FDS / Ruuvi interfaces.
SIM         := $(ROOT)/tests/sim
DRV         := $(ROOT)/src/ruuvi.drivers.c/src
SIM_SOURCES := $(SIM)/sim_main.c $(SIM)/sim_platform.c $(SIM)/sim_softdevice.c \
               $(HK)/app_homekit.c $(HK)/homekit_db.c $(HK)/crypto/HAPOberon.c \
               $(HK)/pal/HAPPlatform.c $(HK)/pal/HAPPlatformBLEPeripheralManager.c \
               $(HK)/pal/HAPPlatformKeyValueStore.c $(HK)/pal/HAPPlatformTimer.c \
               $(filter-out $(ADK)/HAP/HAPIP%.c $(ADK)/HAP/HAPJSONUtils.c,$(wildcard $(ADK)/HAP/*.c)) \
               $(ADK)/PAL/HAPAssert.c $(ADK)/PAL/HAPLog.c $(wildcard $(ADK)/PAL/HAPBase+*.c) \
               $(DRV)/ruuvi_driver_sensor.c
SIM_CFLAGS  := -I$(SIM)/include -I$(SIM) -I$(HK)/pal $(filter-out -I$(ADK)/PAL/Linux,$(CFLAGS)) -I$(BUILD)/test-setup \
               -I$(ADK)/External/JSON -I$(ADK)/External/Base64 \
               -I$(SDK_ROOT)/components/softdevice/s132/headers \
               -I$(DRV) $(addprefix -I,$(wildcard $(DRV)/interfaces/*)) \
               -I$(DRV)/nrf5_sdk15_platform/ruuvi.nrf_sdk15_3_overrides.c \
               -I$(SDK_ROOT)/components/libraries/util \
               -DSVCALL_AS_NORMAL_FUNCTION  \
               -Wno-unused-local-typedefs -Wno-unused-function -Wno-format-truncation

$(BUILD)/hk_sim: $(SIM_SOURCES) $(wildcard $(SIM)/*.h $(SIM)/include/*.h $(HK)/*.h $(HK)/pal/*.h) $(HOST_OBERON) $(BUILD)/test-setup/homekit_setup_info.h
	@$(CC) $(SIM_CFLAGS) -o $@ $(SIM_SOURCES) $(HOST_OBERON) -lm $(quiet_enums)

.PHONY: all run clean
all: run

$(BUILD):
	mkdir -p $@

# nrf_oberon is tagged as an M-profile library. The code itself is plain
# Thumb-2 which the A-profile cores execute fine, so drop the attribute
# section to let the Linux linker accept it.
$(HOST_OBERON): $(OBERON_LIB) | $(BUILD)
	rm -rf $(BUILD)/oberon && mkdir -p $(BUILD)/oberon
	cd $(BUILD)/oberon && $(EMBED_AR) x $(OBERON_LIB)
	for o in $(BUILD)/oberon/*.obj; do $(OBJCOPY) --remove-section .ARM.attributes $$o; done
	$(AR) rcs $@ $(BUILD)/oberon/*.obj

# Apple's own crypto test vectors, run against the Oberon backend.
$(BUILD)/crypto_test: $(ADK)/Tests/HAPCryptoTest.c $(HK)/crypto/HAPOberon.c $(ADK_BASE) $(HOST_OBERON)
	$(CC) $(CFLAGS) -Wno-unused-function -o $@ $(filter %.c,$^) $(HOST_OBERON) $(quiet_enums)

# Checks the SRP salt/verifier produced by tools/homekit_provision.py.
# Code and setup ID are Apple's public HomeKit ADK test vector
# (Tests/HAPAccessorySetupGetSetupPayloadTest.c), not a real accessory's code.
TEST_SETUP_CODE := 518-08-582
$(BUILD)/test-setup/homekit_setup_info.h: $(ROOT)/tools/homekit_provision.py | $(BUILD)
	python3 $< header $@ --state $(BUILD)/test-setup.json --code $(TEST_SETUP_CODE) --setup-id 7OSX --new

$(BUILD)/setup_info_test: setup_info_test.c $(HK)/crypto/HAPOberon.c $(ADK_BASE) $(HOST_OBERON) $(BUILD)/test-setup/homekit_setup_info.h
	$(CC) $(CFLAGS) -I$(BUILD)/test-setup -DHOMEKIT_TEST_SETUP_CODE='"$(TEST_SETUP_CODE)"' \
		-o $@ $(filter %.c,$^) $(HOST_OBERON) $(quiet_enums)

run: $(addprefix $(BUILD)/,$(TESTS)) $(BUILD)/hk_sim
	@for t in $(TESTS); do \
		echo "== $$t"; \
		$(RUN) $(BUILD)/$$t || { echo "FAIL: $$t"; exit 1; }; \
	done
	@echo "All host tests passed."

clean:
	rm -rf $(BUILD)
