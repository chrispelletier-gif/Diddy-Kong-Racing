# 3DS specific build steps: the vertex shader and the .3dsx/.smdh/.cia packaging.

ifeq ($(TARGET),3ds)

TOOLS_BIN   := $(DEVKITPRO)/tools/bin
SHADER_SRC  := 3ds/shader.v.pica 3ds/transform.v.pica
SHADER_BIN  := $(BUILD_DIR)/shader/shader.shbin
SHADER_ASM  := $(BUILD_DIR)/shader/shader_shbin.s
SHADER_HDR  := $(BUILD_DIR)/shader/shader_shbin.h
SHADER_OBJS := $(BUILD_DIR)/shader/shader_shbin.o

APP_TITLE       := Diddy Kong Racing
APP_DESCRIPTION := Native port of the DKR decompilation, version $(PORT_VERSION)
APP_AUTHOR      := DKR decomp contributors
APP_ICON        := $(firstword $(wildcard 3ds/icon.png) $(CTRULIB)/default_icon.png)

# The installable .cia (roadmap O7): `make -C port TARGET=3ds cia`. makerom and bannertool are not part of
# devkitPro; CI downloads them, or point these at your own copies.
MAKEROM         ?= makerom
BANNERTOOL      ?= bannertool
CIA_DIR         := 3ds/cia

SYS_CFLAGS += -I$(BUILD_DIR)/shader

$(SHADER_BIN): $(SHADER_SRC)
	@mkdir -p $(dir $@)
	@echo "PICASSO $^"
	@$(TOOLS_BIN)/picasso -o $@ $^

$(SHADER_ASM) $(SHADER_HDR): $(SHADER_BIN)
	@cd $(dir $<) && $(TOOLS_BIN)/bin2s -a 4 -H shader_shbin.h shader.shbin > shader_shbin.s

$(SHADER_OBJS): $(SHADER_ASM)
	@$(CC) $(ARCH) -x assembler-with-cpp -c $< -o $@

# The renderer includes the generated header.
$(call obj,3ds/gfx_c3d.c): $(SHADER_HDR)

$(EXE): $(SHADER_OBJS)

# Rewritten only when the version changes, so the files that show it are rebuilt then (an incremental build would
# otherwise keep the old one).
VERSION_STAMP := $(BUILD_DIR)/version.txt
$(shell mkdir -p $(BUILD_DIR) && echo '$(PORT_VERSION) $(PORT_BUILD_VERSION)' | cmp -s - $(VERSION_STAMP) || \
        echo '$(PORT_VERSION) $(PORT_BUILD_VERSION)' > $(VERSION_STAMP))
$(foreach f,src/plat/plat_common.c 3ds/bugreport_3ds.c 3ds/update_3ds.c,$(call obj,$(f))): $(VERSION_STAMP)

$(BUILD_DIR)/dkr.smdh: $(APP_ICON) $(VERSION_STAMP)
	@$(TOOLS_BIN)/smdhtool --create "$(APP_TITLE)" "$(APP_DESCRIPTION)" "$(APP_AUTHOR)" $(APP_ICON) $@

$(BUILD_DIR)/dkr.3dsx: $(EXE) $(BUILD_DIR)/dkr.smdh
	@echo "3DSX $@"
	@$(TOOLS_BIN)/3dsxtool $(EXE) $@ --smdh=$(BUILD_DIR)/dkr.smdh

# The HOME menu banner: a flat 256x128 picture and a short sound, both original art (port/3ds/cia/README.md).
$(BUILD_DIR)/dkr.bnr: $(CIA_DIR)/banner.png $(CIA_DIR)/banner.wav
	@$(BANNERTOOL) makebanner -i $(CIA_DIR)/banner.png -a $(CIA_DIR)/banner.wav -o $@

$(BUILD_DIR)/dkr.cia: $(EXE) $(BUILD_DIR)/dkr.smdh $(BUILD_DIR)/dkr.bnr $(CIA_DIR)/dkr.rsf
	@echo "CIA $@"
	@$(MAKEROM) -f cia -o $@ -elf $(EXE) -rsf $(CIA_DIR)/dkr.rsf -icon $(BUILD_DIR)/dkr.smdh \
		-banner $(BUILD_DIR)/dkr.bnr -exefslogo -target t \
		-major $(PORT_VERSION_MAJOR) -minor $(PORT_VERSION_MINOR) -micro $(PORT_VERSION_MICRO)

.PHONY: cia
cia: $(BUILD_DIR)/dkr.cia

endif
