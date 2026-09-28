# The native reference: the SAME adapter sources as guest.mk (the exports
# included - run-native drives them directly), compiled with exactly the flags
# CMake gave Azahar's core library (extract-tu-flags.py mines them), linked
# against everything build/native produced. Run build-native.sh first.
#
# Usage: make -f native.mk -j$(nproc)

ROOT := ..
B    := $(ROOT)/build/native
O    := obj-native
MB   ?= $(HOME)/chimera/extern/chimera-common-minibox

TUFLAGS  := $(shell python3 extract-tu-flags.py $(B)/compile_commands.json src/core/core.cpp)
MBINCS   := -Inative-shim -I$(MB)/source/guest/include -I$(MB)/extern/jsmn
GLINCS   := -I$(MB)/source/gl -I$(ROOT)/extern/azahar/externals/glad/include -Iglad/include -Igenerated-gl
CXXFLAGS := -O2 -g1 $(TUFLAGS) $(MBINCS) $(GLINCS) -I.
CFLAGS   := -O2 -g1 $(MBINCS) -I.

LIBS := $(shell find $(B) -name '*.a' | sort)
HDRS := azahar-driver.h chimera-fs-host.h gate-harness.h zip-read.h gl-shim.h
GEN  := generated-gl/gl-bridge-guest.cpp
OBJS := $(O)/run-native.o $(O)/wbx-entry.o $(O)/azahar-driver.o $(O)/chimera-fs.o $(O)/zip-read.o \
        $(O)/gl-shim.o $(O)/gl-bridge-guest.o $(O)/gl-host-renamed.o

all: $(O)/run-native

# One run of gen-gl-bridge.py writes all three generated files (build-core.sh
# runs the same command); every object that includes one waits for it.
$(GEN): gl-entry-points.txt $(MB)/source/gl/gl-entry-points.txt glad/include/glad/gl.h
	mkdir -p generated-gl
	python3 $(MB)/source/gl/gen-gl-bridge.py glad/include/glad/gl.h \
		$(MB)/source/gl/gl-entry-points.txt generated-gl --only gl-entry-points.txt

$(O)/%.o: %.cpp $(HDRS) $(GEN)
	@mkdir -p $(O)
	g++ $(CXXFLAGS) -c -o $@ $<

$(O)/gl-bridge-guest.o: $(GEN)
	@mkdir -p $(O)
	g++ $(CXXFLAGS) -c -o $@ $<

$(O)/run-native.o: run-native.c $(HDRS)
	@mkdir -p $(O)
	gcc $(CFLAGS) -DCHIMERA_GL_BRIDGE -I$(MB)/source/gl -c -o $@ $<

# The host half, as run-wbx has it - but in this ONE binary Azahar's own glad
# (the guest's) already defines every glad_gl* pointer, so the host's glad 2
# pointers and loader are renamed hglad_* before they are linked beside it.
$(O)/gl-host-renamed.o: gl-host.c glad/src/gl.c $(GEN)
	@mkdir -p $(O)
	gcc -O2 -DCHIMERA_GL_BRIDGE $(GLINCS) -c -o $(O)/gl-host.o gl-host.c
	gcc -O2 $(GLINCS) -c -o $(O)/glad2-gl.o glad/src/gl.c
	ld -r -o $(O)/gl-host-both.o $(O)/gl-host.o $(O)/glad2-gl.o
	nm $(O)/gl-host-both.o | awk '$$NF ~ /^glad_|^gladLoad|^gladSet|^gladGet|^gladInstall|^gladUninstall|^GLAD_/ {print $$NF, "h" $$NF}' | sort -u > $(O)/glad-rename.map
	objcopy --redefine-syms=$(O)/glad-rename.map $(O)/gl-host-both.o $@

$(O)/run-native: $(OBJS) $(LIBS)
	g++ -o $@ $(OBJS) -Wl,--start-group $(LIBS) -Wl,--end-group -lpthread -lm -ldl -lrt -lEGL

clean:
	rm -rf $(O)

.PHONY: all clean
