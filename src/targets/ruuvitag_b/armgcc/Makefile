PROJECT_NAME     := ruuvitag_b_s132
TARGETS          := nrf52832_xxaa
OUTPUT_DIRECTORY := _build

SDK_ROOT := ../../../../nRF5_SDK_15.3.0_59ac345
PROJ_DIR := ../../..
.DEFAULT_GOAL := default

RUUVI_LINKER_SCRIPT ?= ruuvitag_b.ld
HEAP_SIZE ?= 8192

include ${PROJ_DIR}/gcc_sources.make

# Native HomeKit variant (make HOMEKIT=1 MODE=-DAPPLICATION_MODE_HOMEKIT ...)
ifeq ($(HOMEKIT),1)
include ${PROJ_DIR}/homekit/homekit_sources.make
COMMON_SOURCES += $(HOMEKIT_SOURCES)
COMMON_INCLUDES += $(HOMEKIT_INCLUDES)
CFLAGS += $(HOMEKIT_CFLAGS)
# The larger HomeKit GATT database needs more SoftDevice RAM.
RUUVI_LINKER_SCRIPT := ruuvitag_b_homekit.ld
# Nothing in the HomeKit variant allocates from the heap.
HEAP_SIZE := 2048
# All objects may include the generated setup header through HAPPlatform.c.
$(OUTPUT_DIRECTORY)/nrf52832_xxaa/HAPPlatform.c.o: $(HOMEKIT_SETUP_HEADER)
endif

$(OUTPUT_DIRECTORY)/nrf52832_xxaa.out: \
  LINKER_SCRIPT  := $(RUUVI_LINKER_SCRIPT)

# Source files common to all targets
SRC_FILES += ${COMMON_SOURCES} ${NFC_SOURCES} \
   $(SDK_ROOT)/modules/nrfx/mdk/gcc_startup_nrf52.S \
   $(SDK_ROOT)/modules/nrfx/mdk/system_nrf52.c

# Include folders common to all targets
INC_FOLDERS += ${COMMON_INCLUDES}

# Libraries common to all targets
LIB_FILES += \
 $(NFC_T4T_FPU_BIN) \
 $(SDK_ROOT)/external/nrf_oberon/lib/cortex-m4/hard-float/liboberon_2.0.7.a

# Optimization flags
OPT ?= -O3 -g3
# Uncomment the line below to enable link time optimization
#OPT += -flto

CFLAGS += $(MODE)
CFLAGS += $(RUN_TESTS)
CFLAGS += $(DEBUG)
CFLAGS += $(FW_VERSION)
CFLAGS += $(OPT)
CFLAGS += $(COMMON_DEFINES)
CFLAGS += -DBL_SETTINGS_ACCESS_ONLY
CFLAGS += -DBOARD_RUUVITAG_B
CFLAGS += -DCONFIG_GPIO_AS_PINRESET
CFLAGS += -DFLOAT_ABI_HARD
CFLAGS += -DBME280_FLOAT_ENABLE
CFLAGS += -DNRF52
CFLAGS += -DNRF52832_XXAA
CFLAGS += -DNRF52_PAN_74
CFLAGS += -DNRF_DFU_SVCI_ENABLED
CFLAGS += -DNRF_DFU_TRANSPORT_BLE=1
CFLAGS += -DNRF_SD_BLE_API_VERSION=6
CFLAGS += -DS132
CFLAGS += -DSOFTDEVICE_PRESENT
CFLAGS += -DNRF_CRYPTO_MAX_INSTANCE_COUNT=1
CFLAGS += -DuECC_ENABLE_VLI_API=0
CFLAGS += -DuECC_OPTIMIZATION_LEVEL=3
CFLAGS += -DuECC_SQUARE_FUNC=0
CFLAGS += -DuECC_SUPPORT_COMPRESSED_POINT=0
CFLAGS += -DuECC_VLI_NATIVE_LITTLE_ENDIAN=1
CFLAGS += -DSWI_DISABLE0
CFLAGS += -mcpu=cortex-m4
CFLAGS += -mthumb -mabi=aapcs
CFLAGS += -Wall -Werror
# nRF5 SDK 15.3 predates GCC 12+ array bounds analysis.
CFLAGS += -Wno-array-bounds -Wno-array-parameter -Wno-stringop-overflow
CFLAGS += -mfloat-abi=hard -mfpu=fpv4-sp-d16
# keep every function in a separate section, this allows linker to discard unused ones
CFLAGS += -ffunction-sections -fdata-sections -fno-strict-aliasing -fstack-usage
CFLAGS += -fno-builtin -fshort-enums 

# C++ flags common to all targets
CXXFLAGS += $(OPT)

# Assembler flags common to all targets
ASMFLAGS += -g3
ASMFLAGS += -mcpu=cortex-m4
ASMFLAGS += -mthumb -mabi=aapcs
ASMFLAGS += -mfloat-abi=hard -mfpu=fpv4-sp-d16
ASMFLAGS += -DBL_SETTINGS_ACCESS_ONLY
ASMFLAGS += -DBOARD_RUUVITAG_B
ASMFLAGS += -DBOARD_CUSTOM
ASMFLAGS += -DCONFIG_GPIO_AS_PINRESET
ASMFLAGS += -DFLOAT_ABI_HARD
ASMFLAGS += -DBME280_FLOAT_ENABLE
ASMFLAGS += -DNRF52
ASMFLAGS += -DNRF52832_XXAA
ASMFLAGS += -DHAL_NFC_ENGINEERING_BC_FTPAN_WORKAROUND
ASMFLAGS += -DNRF52_PAN_74
ASMFLAGS += -DNRF_DFU_SVCI_ENABLED
ASMFLAGS += -DNRF_DFU_TRANSPORT_BLE=1
ASMFLAGS += -DNRF_SD_BLE_API_VERSION=6
ASMFLAGS += -DS132
ASMFLAGS += -DSOFTDEVICE_PRESENT
ASMFLAGS += -DNRF_CRYPTO_MAX_INSTANCE_COUNT=1
ASMFLAGS += -DuECC_ENABLE_VLI_API=0
ASMFLAGS += -DuECC_OPTIMIZATION_LEVEL=3
ASMFLAGS += -DuECC_SQUARE_FUNC=0
ASMFLAGS += -DuECC_SUPPORT_COMPRESSED_POINT=0
ASMFLAGS += -DuECC_VLI_NATIVE_LITTLE_ENDIAN=1
ASMFLAGS += -DSWI_DISABLE0

# Linker flags
LDFLAGS += $(OPT)
LDFLAGS += -mthumb -mabi=aapcs -L$(SDK_ROOT)/modules/nrfx/mdk -T$(LINKER_SCRIPT)
LDFLAGS += -mcpu=cortex-m4
LDFLAGS += -mfloat-abi=hard -mfpu=fpv4-sp-d16
# let linker dump unused sections
LDFLAGS += -Wl,--gc-sections
# use newlib in nano version
LDFLAGS += --specs=nano.specs
LDFLAGS += -u _printf_float

nrf52832_xxaa: CFLAGS += -D__HEAP_SIZE=$(HEAP_SIZE)
nrf52832_xxaa: CFLAGS += -D__STACK_SIZE=8192
nrf52832_xxaa: ASMFLAGS += -D__HEAP_SIZE=$(HEAP_SIZE)
nrf52832_xxaa: ASMFLAGS += -D__STACK_SIZE=8192

# Add standard libraries at the very end of the linker input, after all objects
# that may need symbols provided by these libraries.
LIB_FILES += -lc -lnosys -lm


.PHONY: default help

# Default target - first one defined
default: nrf52832_xxaa

# Print all targets that can be built
help:
	@echo following targets are available:
	@echo		nrf52832_xxaa

TEMPLATE_PATH := $(SDK_ROOT)/components/toolchain/gcc


include $(TEMPLATE_PATH)/Makefile.common

$(foreach target, $(TARGETS), $(call define_target, $(target)))
