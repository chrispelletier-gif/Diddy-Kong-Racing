name := ares-headless
build := optimized
threaded := true
openmp := false
vulkan := true
local := true
lto := false
flags += -I. -I.. -I../ares -I../thirdparty -DMIA_LIBRARY

nall.path := ../nall
include $(nall.path)/GNUmakefile

libco.path := ../libco
include $(libco.path)/GNUmakefile

thirdparty.path := ../thirdparty
sljit.path := $(thirdparty.path)/sljit/sljit_src
libchdr.path := $(thirdparty.path)/libchdr
tzxfile.path := $(thirdparty.path)/TZXFile
ymfm.path := $(thirdparty.path)/ymfm
include $(thirdparty.path)/GNUmakefile

profile := accuracy
cores := n64

ares.path := ../ares
include $(ares.path)/GNUmakefile

mia.path := ../mia
mia.objects := mia mia-resource
mia.objects := $(mia.objects:%=$(object.path)/%.o)
$(object.path)/mia.o: $(mia.path)/mia.cpp
$(object.path)/mia-resource.o: $(mia.path)/resource/resource.cpp

headless.objects := $(object.path)/headless.o
$(object.path)/headless.o: headless.cpp

all.objects := $(libco.objects) $(sljit.objects) $(tzxfile.objects) $(ymfm.objects) $(nall.objects) $(ares.objects) $(mia.objects) $(headless.objects)
all.options := $(libco.options) $(sljit.options) $(tzxfile.options) $(ymfm.options) $(nall.options) $(ares.options) $(mia.options) $(options) -lz -ldl -lchdr

$(all.objects): | $(object.path)

all: $(all.objects) | $(output.path)
	$(info Linking $(output.path)/$(name)$(extension) ...)
	+@$(compiler) $(call exe,$(output.path)/$(name)$(extension)) $(all.objects) $(all.options)

-include $(object.path)/*.d
