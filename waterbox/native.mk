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
CXXFLAGS := -O2 -g1 $(TUFLAGS) $(MBINCS) -I.
CFLAGS   := -O2 -g1 $(MBINCS) -I.

LIBS := $(shell find $(B) -name '*.a' | sort)
HDRS := azahar-driver.h chimera-fs-host.h gate-harness.h zip-read.h

all: $(O)/run-native

$(O)/%.o: %.cpp $(HDRS)
	@mkdir -p $(O)
	g++ $(CXXFLAGS) -c -o $@ $<

$(O)/run-native.o: run-native.c $(HDRS)
	@mkdir -p $(O)
	gcc $(CFLAGS) -c -o $@ $<

$(O)/run-native: $(O)/run-native.o $(O)/wbx-entry.o $(O)/azahar-driver.o $(O)/chimera-fs.o $(O)/zip-read.o $(LIBS)
	g++ -o $@ $(O)/run-native.o $(O)/wbx-entry.o $(O)/azahar-driver.o $(O)/chimera-fs.o $(O)/zip-read.o \
		-Wl,--start-group $(LIBS) -Wl,--end-group -lpthread -lm -ldl -lrt

clean:
	rm -rf $(O)

.PHONY: all clean
