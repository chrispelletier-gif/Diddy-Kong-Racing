# 3DS specific build steps: the vertex shader and the .3dsx/.smdh packaging.

ifeq ($(TARGET),3ds)

TOOLS_BIN   := $(DEVKITPRO)/tools/bin
SHADER_SRC  := 3ds/shader.v.pica
SHADER_BIN  := $(BUILD_DIR)/shader/shader.shbin
SHADER_ASM  := $(BUILD_DIR)/shader/shader_shbin.s
SHADER_HDR  := $(BUILD_DIR)/shader/shader_shbin.h
SHADER_OBJS := $(BUILD_DIR)/shader/shader_shbin.o

APP_TITLE       := Diddy Kong Racing
APP_DESCRIPTION := Native port of the DKR decompilation
APP_AUTHOR      := DKR decomp contributors
APP_ICON        := $(firstword $(wildcard 3ds/icon.png) $(CTRULIB)/default_icon.png)

SYS_CFLAGS += -I$(BUILD_DIR)/shader

$(SHADER_BIN): $(SHADER_SRC)
	@mkdir -p $(dir $@)
	@echo "PICASSO $<"
	@$(TOOLS_BIN)/picasso -o $@ $<

$(SHADER_ASM) $(SHADER_HDR): $(SHADER_BIN)
	@cd $(dir $<) && $(TOOLS_BIN)/bin2s -a 4 -H shader_shbin.h shader.shbin > shader_shbin.s

$(SHADER_OBJS): $(SHADER_ASM)
	@$(CC) $(ARCH) -x assembler-with-cpp -c $< -o $@

# The renderer includes the generated header.
$(call obj,3ds/gfx_c3d.c): $(SHADER_HDR)

$(EXE): $(SHADER_OBJS)

$(BUILD_DIR)/dkr.smdh:
	@$(TOOLS_BIN)/smdhtool --create "$(APP_TITLE)" "$(APP_DESCRIPTION)" "$(APP_AUTHOR)" $(APP_ICON) $@

$(BUILD_DIR)/dkr.3dsx: $(EXE) $(BUILD_DIR)/dkr.smdh
	@echo "3DSX $@"
	@$(TOOLS_BIN)/3dsxtool $(EXE) $@ --smdh=$(BUILD_DIR)/dkr.smdh

endif
