# Native port of Diddy Kong Racing.
#
#   make -C port TARGET=3ds    builds port/build/3ds/dkr.3dsx (needs devkitARM + libctru + citro3d)
#   make -C port TARGET=host   builds port/build/host/dkr, a headless PC build used for testing
#
# Both builds need include/asset_enums.h, which the asset tool generates from your ROM:
#   make -C tools dkr_assets_tool && ./tools/dkr_assets_tool extract -dkrv us.v77 &&
#   ./tools/dkr_assets_tool build -o build/assets.bin -dkrv us.v77

TARGET  ?= host
REGION  ?= us
VERSION ?= v77
DEBUG   ?= 0

ROOT      := ..
BUILD_DIR := build/$(TARGET)

ifeq ($(wildcard $(ROOT)/include/asset_enums.h),)
$(error include/asset_enums.h is missing. Extract and build the assets from your ROM first (see port/README.md))
endif

#### Sources ####

GAME_SRC := $(wildcard $(ROOT)/src/*.c) $(ROOT)/src/hasm/collision.c $(ROOT)/src/hasm/math_util.c

LIBULTRA_SRC := \
    $(wildcard $(ROOT)/libultra/src/audio/*.c) \
    $(wildcard $(ROOT)/libultra/src/audio/mips1/*.c) \
    $(ROOT)/libultra/src/sc/sched.c \
    $(ROOT)/libultra/src/gu/mtxutil.c \
    $(ROOT)/libultra/src/gu/perspective.c \
    $(ROOT)/libultra/src/gu/sinf.c \
    $(ROOT)/libultra/src/gu/cosf.c \
    $(ROOT)/libultra/src/libc/string.c \
    $(ROOT)/libultra/src/libc/xprintf.c \
    $(ROOT)/libultra/src/libc/xldtob.c \
    $(ROOT)/libultra/src/libc/xlitob.c \
    $(ROOT)/libultra/src/libc/ldiv.c \
    $(ROOT)/libultra/src/io/vitbl.c \
    $(ROOT)/libultra/src/io/contpfs.c \
    $(ROOT)/libultra/src/io/crc.c \
    $(filter-out %/pfsinit.c %/pfsisplug.c,$(wildcard $(ROOT)/libultra/src/io/pfs*.c))

# Port sources that are compiled like game code (libultra headers, no system headers).
PORT_GAME_SRC := $(wildcard src/*.c src/os/*.c src/game/*.c src/gfx/*.c src/audio/*.c)
# Port sources that use the platform's own headers.
PORT_SYS_SRC := src/plat/plat_common.c src/plat/input_script.c $(wildcard src/gfx/sys/*.c src/audio/sys/*.c)

ifeq ($(TARGET),3ds)
  PORT_SYS_SRC += src/plat/plat_3ds.c $(wildcard 3ds/*.c)
else
  PORT_SYS_SRC += src/plat/plat_posix.c $(wildcard host/*.c)
endif

#### Flags ####

GAME_DEFINES := -DTARGET_PORT=1 -DNON_MATCHING=1 -DAVOID_UB=1 -D_LANGUAGE_C -D_FINALROM -DNDEBUG -DF3DDKR_GBI \
                -DVERSION_$(REGION)_$(VERSION) -DBUILD_VERSION=VERSION_G -DBUILD_VERSION_STRING=\"2.0G\" \
                -D_MIPS_SZLONG=32
# libultra names that clash with libctru/newlib symbols.
GAME_DEFINES += -DosGetTime=n64_osGetTime -D__assert=n64___assert $(EXTRA_DEFINES)

GAME_INCLUDES := -I$(ROOT) -I$(ROOT)/include -I$(ROOT)/include/libc -I$(ROOT)/include/PR -I$(ROOT)/include/sys \
                 -I$(ROOT)/src -I$(ROOT)/libultra -I$(ROOT)/libultra/src/gu -I$(ROOT)/libultra/src/libc \
                 -I$(ROOT)/libultra/src/io -I$(ROOT)/libultra/src/sc -I$(ROOT)/libultra/src/audio \
                 -I$(ROOT)/libultra/src/os -I$(ROOT)/src/hasm -Iinclude -Isrc

# The N64 compiler treats char as unsigned and plain C semantics for overflow/aliasing are relied upon.
COMMON_FLAGS := -funsigned-char -fno-strict-aliasing -fwrapv -fno-common -fno-stack-protector
# DEBUG=asan: AddressSanitizer, plus the float to integer conversions that behave differently on ARM.
SANITIZE_FLAGS := -fsanitize=address,float-cast-overflow
ifeq ($(DEBUG),1)
  OPT_FLAGS := -O0 -g
else ifeq ($(DEBUG),asan)
  OPT_FLAGS := -O1 -g $(SANITIZE_FLAGS) -fno-omit-frame-pointer
else
  OPT_FLAGS := -O2 -g
endif

ifeq ($(TARGET),3ds)
  ifeq ($(strip $(DEVKITARM)),)
    DEVKITPRO ?= /opt/devkitpro
    DEVKITARM ?= $(DEVKITPRO)/devkitARM
  endif
  DEVKITPRO ?= /opt/devkitpro
  PREFIX    := $(DEVKITARM)/bin/arm-none-eabi-
  CC        := $(PREFIX)gcc
  ARCH      := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
  # The ARM EABI default of variable sized enums would change the layout of the game's structs.
  GAME_ARCH_FLAGS := -fno-short-enums
  CTRULIB   := $(DEVKITPRO)/libctru
  SYS_CFLAGS := $(ARCH) -D__3DS__ -I$(CTRULIB)/include -Iinclude -Isrc
  GAME_SYSINC := -nostdinc -isystem $(shell $(CC) -print-file-name=include)
  LDFLAGS   := -specs=3dsx.specs $(ARCH) -L$(CTRULIB)/lib -Wl,-Map,$(BUILD_DIR)/dkr.map -Wl,--no-enum-size-warning
  LIBS      := -lcitro3d -lctru -lm
  EXE       := $(BUILD_DIR)/dkr.elf
else
  CC        := gcc
  ARCH      := -m32 -msse2 -mfpmath=sse
  SYS_CFLAGS := $(ARCH) -Iinclude -Isrc
  GAME_SYSINC := -nostdinc -isystem $(shell $(CC) $(ARCH) -print-file-name=include)
  LDFLAGS   := $(ARCH) $(if $(filter asan,$(DEBUG)),$(SANITIZE_FLAGS))
  LIBS      := -lm -lpthread
  EXE       := $(BUILD_DIR)/dkr
endif

GAME_CFLAGS := $(ARCH) $(GAME_ARCH_FLAGS) $(OPT_FLAGS) $(COMMON_FLAGS) -std=gnu99 $(GAME_SYSINC) $(GAME_INCLUDES) $(GAME_DEFINES) \
               -include port_swap.h -w -Werror=implicit-function-declaration
PORT_CFLAGS := $(ARCH) $(GAME_ARCH_FLAGS) $(OPT_FLAGS) $(COMMON_FLAGS) -std=gnu99 $(GAME_SYSINC) $(GAME_INCLUDES) $(GAME_DEFINES) \
               -Wall -Wno-unused-function -Wno-missing-braces -Werror=implicit-function-declaration
SYS_CFLAGS  += $(OPT_FLAGS) $(COMMON_FLAGS) -std=gnu99 -Wall -Wno-unused-function

#### Rules ####

obj = $(BUILD_DIR)/obj/$(subst ../,,$(1:.c=.o))

GAME_OBJS     := $(foreach f,$(GAME_SRC) $(LIBULTRA_SRC),$(call obj,$(f)))
PORT_GAME_OBJS := $(foreach f,$(PORT_GAME_SRC),$(call obj,$(f)))
PORT_SYS_OBJS := $(foreach f,$(PORT_SYS_SRC),$(call obj,$(f)))
ALL_OBJS      := $(GAME_OBJS) $(PORT_GAME_OBJS) $(PORT_SYS_OBJS) $(SHADER_OBJS)

.PHONY: all clean
all: $(if $(filter 3ds,$(TARGET)),$(BUILD_DIR)/dkr.3dsx,$(EXE))

define compile_rule
$(call obj,$(1)): $(1)
	@mkdir -p $$(dir $$@)
	@echo "CC $(1)"
	@$$(CC) $(2) -MMD -MP -c $$< -o $$@
endef

$(foreach f,$(GAME_SRC) $(LIBULTRA_SRC),$(eval $(call compile_rule,$(f),$$(GAME_CFLAGS))))
$(foreach f,$(PORT_GAME_SRC),$(eval $(call compile_rule,$(f),$$(PORT_CFLAGS))))
$(foreach f,$(PORT_SYS_SRC),$(eval $(call compile_rule,$(f),$$(SYS_CFLAGS))))

$(EXE): $(ALL_OBJS)
	@echo "LD $@"
	@$(CC) $(LDFLAGS) $^ $(LIBS) -o $@

-include 3ds/rules.mk

clean:
	rm -rf $(BUILD_DIR)

-include $(ALL_OBJS:.o=.d)
