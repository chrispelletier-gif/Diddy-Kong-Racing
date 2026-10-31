# Native port of Diddy Kong Racing.
#
#   make -C port TARGET=3ds    builds port/build/3ds/dkr.3dsx (needs devkitARM + libctru + citro3d)
#   make -C port TARGET=host   builds port/build/host/dkr, a headless PC build used for testing
#   RELEASE=1                  a build for players (port/build/3ds-release/): the Settings menus leave out the
#                              performance switches and the debug tools (plat.h PLAT_DEBUG_TOOLS)
#
# Both builds need include/asset_enums.h, which the asset tool generates from your ROM:
#   make -C tools dkr_assets_tool && ./tools/dkr_assets_tool extract -dkrv us.v77 &&
#   ./tools/dkr_assets_tool build -o build/assets.bin -dkrv us.v77

TARGET  ?= host
REGION  ?= us
VERSION ?= v77
DEBUG   ?= 0
RELEASE ?= 0

ROOT      := ..
BUILD_DIR := build/$(TARGET)$(if $(filter 1,$(RELEASE)),-release)

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
PORT_SYS_SRC += src/plat/settings.c src/plat/controls.c src/plat/bottom_screen.c src/plat/options.c \
                src/plat/save_backups.c src/plat/recorder.c

ifeq ($(TARGET),3ds)
  PORT_SYS_SRC += src/plat/plat_3ds.c src/plat/bugreport.c $(wildcard 3ds/*.c)
else
  PORT_SYS_SRC += src/plat/plat_posix.c $(wildcard host/*.c)
endif

# A player's build (RELEASE=1) leaves the debug tools out: the profiler, the test runs, bug reports (crash.txt is still
# written), the updater and the reference renderer. src/release/ stands in for what the rest of the port calls.
ifeq ($(RELEASE),1)
  PORT_GAME_SRC := $(filter-out src/game/port_autorun.c src/game/port_profile.c src/game/port_recorder.c,$(PORT_GAME_SRC)) \
                   src/release/debug_stubs.c
  PORT_SYS_SRC  := $(filter-out src/plat/bugreport.c src/plat/recorder.c 3ds/net_3ds.c 3ds/gfx_ref.c,$(PORT_SYS_SRC))
endif

#### Flags ####

GAME_DEFINES := -DTARGET_PORT=1 -DNON_MATCHING=1 -DAVOID_UB=1 -D_LANGUAGE_C -D_FINALROM -DNDEBUG -DF3DDKR_GBI \
                -DVERSION_$(REGION)_$(VERSION) -DBUILD_VERSION=VERSION_G -DBUILD_VERSION_STRING=\"2.0G\" \
                -D_MIPS_SZLONG=32
# libultra names that clash with libctru/newlib symbols.
GAME_DEFINES += -DosGetTime=n64_osGetTime -D__assert=n64___assert $(EXTRA_DEFINES)
# PORT_MULTIPLAYER=1 brings back the hidden 2 to 4 player game (port/include/port.h).
ifeq ($(PORT_MULTIPLAYER),1)
  GAME_DEFINES += -DPORT_MULTIPLAYER=1
endif
# RELEASE=1: the menus show only what players need; the tools stay compiled in (roadmap P14) but out of reach.
RELEASE_DEFINES := $(if $(filter 1,$(RELEASE)),-DPORT_RELEASE=1)
GAME_DEFINES += $(RELEASE_DEFINES)

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
  # -O3 with link-time optimisation across the game, libultra and the port (roadmap P11). Neither changes how floats
  # are rounded, so the game plays exactly as with -O2 (checked with port/tools/frame_regression.py).
  # OPT=2 LTO=0 gives the old build for comparison. HOT_OPT is the level for the loop-heavy renderer and audio files
  # (HOT_SRC), so the rest can stay smaller.
  OPT ?= 3
  HOT_OPT ?= $(OPT)
  LTO ?= 1
  OPT_FLAGS := -O$(OPT) -g $(if $(filter 1,$(LTO)),-flto=auto)
endif
HOT_SRC := src/gfx/gfx_rdp.c src/audio/audio_rsp.c 3ds/gfx_c3d.c
# Roadmap P25: the hot files (only the renderer has floats) may reorder float maths, use reciprocals and skip NaN,
# infinity, signed zero and errno handling, which the ARM11's slow VFP compares and divides pay for. Game code keeps
# strict float maths so physics stays bit-identical. FAST_MATH=0 gives the old build.
FAST_MATH ?= 1
# HOT_EXTRA_FLAGS adds to the hot files' flags (for trying optimisation options on them alone).
HOT_FLAGS := $(if $(HOT_OPT),-O$(HOT_OPT) $(if $(filter 1,$(FAST_MATH)),-ffast-math) $(HOT_EXTRA_FLAGS))

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
  GAME_ARCH_FLAGS := -fno-short-enums $(if $(filter 1,$(RELEASE)),,-DPORT_GFX_REFERENCE)
  CTRULIB   := $(DEVKITPRO)/libctru
  PORTLIBS  := $(DEVKITPRO)/portlibs/3ds
  GIT := git -c safe.directory='*'
  # The version comes from the newest vX.Y.Z[-label] tag (a tagged release, .github/workflows/Port.yml): exactly the
  # tag's "0.1.0-beta" on the tagged commit, "0.1.0-beta+3" three commits later, "dev" before the first tag.
  PORT_TAG := $(shell $(GIT) describe --tags --match 'v[0-9]*' --abbrev=0 2>/dev/null)
  PORT_TAG_DISTANCE := $(if $(PORT_TAG),$(shell $(GIT) rev-list --count $(PORT_TAG)..HEAD 2>/dev/null))
  PORT_VERSION := $(if $(PORT_TAG),$(PORT_TAG:v%=%)$(if $(filter-out 0,$(PORT_TAG_DISTANCE)),+$(PORT_TAG_DISTANCE)),dev)
  # The tag's numbers, for the .cia's title version (3ds/rules.mk); 0.0.0 before the first tag.
  PORT_VERSION_NUMBERS := $(subst ., ,$(firstword $(subst -, ,$(subst +, ,$(PORT_TAG:v%=%)))))
  PORT_VERSION_MAJOR := $(or $(word 1,$(PORT_VERSION_NUMBERS)),0)
  PORT_VERSION_MINOR := $(or $(word 2,$(PORT_VERSION_NUMBERS)),0)
  PORT_VERSION_MICRO := $(or $(word 3,$(PORT_VERSION_NUMBERS)),0)
  # Identifies the build in bug reports: git describe from the newest version tag, or the commit before there is one.
  PORT_BUILD_VERSION := $(shell $(GIT) describe --tags --match 'v[0-9]*' --always --dirty 2>/dev/null || echo unknown)$(if $(filter 1,$(RELEASE)),-release)
  # The full commit, which the update (3ds/update_3ds.c) compares with the published builds.
  PORT_BUILD_COMMIT := $(shell $(GIT) rev-parse HEAD 2>/dev/null)
  SYS_CFLAGS := $(ARCH) -D__3DS__ -I$(CTRULIB)/include -I$(PORTLIBS)/include -Iinclude -Isrc \
                -DPORT_VERSION=\"$(PORT_VERSION)\" -DPORT_BUILD_VERSION=\"$(PORT_BUILD_VERSION)\" \
                -DPORT_BUILD_COMMIT=\"$(PORT_BUILD_COMMIT)\"
  GAME_SYSINC := -nostdinc -isystem $(shell $(CC) -print-file-name=include)
  LDFLAGS   := -specs=3dsx.specs $(ARCH) -L$(PORTLIBS)/lib -L$(CTRULIB)/lib -Wl,-Map,$(BUILD_DIR)/dkr.map -Wl,--no-enum-size-warning -Wl,--wrap=gfxScreenSwapBuffers -Wl,--wrap=gfxGetFramebuffer
  # One link-time partition, so that the renderer's per-draw functions (plat.h PLAT_HOT) come out in one .text.hot
  # section: with several, each partition's .text.hot lands among the others' cold code (roadmap O9).
  LDFLAGS   += $(if $(filter 1,$(LTO)),-flto-partition=one)
  # curl and mbedtls (devkitPro's 3ds-curl) send bug reports and download updates, so a player's build has neither.
  LIBS      := -lcitro3d $(if $(filter 1,$(RELEASE)),,-lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz) -lctru -lm
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
SYS_CFLAGS  += $(OPT_FLAGS) $(COMMON_FLAGS) $(RELEASE_DEFINES) -std=gnu99 -Wall -Wno-unused-function

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
	@$$(CC) $(2) $$(FILE_FLAGS) -MMD -MP -c $$< -o $$@
endef

$(foreach f,$(GAME_SRC) $(LIBULTRA_SRC),$(eval $(call compile_rule,$(f),$$(GAME_CFLAGS))))
$(foreach f,$(PORT_GAME_SRC),$(eval $(call compile_rule,$(f),$$(PORT_CFLAGS))))
$(foreach f,$(PORT_SYS_SRC),$(eval $(call compile_rule,$(f),$$(SYS_CFLAGS))))
ifneq ($(strip $(HOT_FLAGS)),)
$(foreach f,$(HOT_SRC),$(call obj,$(f))): FILE_FLAGS := $(HOT_FLAGS)
endif

$(EXE): $(ALL_OBJS)
	@echo "LD $@"
	@$(CC) $(LDFLAGS) $(OPT_FLAGS) $(COMMON_FLAGS) $^ $(LIBS) -o $@

-include 3ds/rules.mk

clean:
	rm -rf $(BUILD_DIR)

-include $(ALL_OBJS:.o=.d)
