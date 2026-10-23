# Builds the replay tool against the host build's renderer (make -C port TARGET=host first).
HOST_OBJ := ../../build/host/obj

build/replay: replay.c $(HOST_OBJ)/src/gfx/gfx_rdp.o $(HOST_OBJ)/host/gfx_soft.o
	mkdir -p build
	$(CC) -m32 -O2 -Wall -o $@ $^ -lm

clean:
	rm -rf build
