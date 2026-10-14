# Builds chimera-test.3dsx, the test script's 3DS program, with devkitARM.
#
#   make OUT=<folder>        (DEVKITPRO and DEVKITARM set, as devkitPro's
#                             own environment file sets them)
#
# devkitPro's own Docker image (devkitpro/devkitarm) has everything this
# needs; that is how CI builds it.
ifeq ($(strip $(DEVKITARM)),)
$(error DEVKITARM is not set: install devkitARM (devkitPro's 3ds-dev) or build in devkitpro/devkitarm)
endif

OUT ?= .
CC := $(DEVKITARM)/bin/arm-none-eabi-gcc
ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS := -g -O2 -Wall -std=gnu11 $(ARCH) -D__3DS__ -I$(DEVKITPRO)/libctru/include
LDFLAGS := -specs=3dsx.specs -g $(ARCH) -L$(DEVKITPRO)/libctru/lib

$(OUT)/chimera-test.3dsx: $(OUT)/chimera-test.elf
	$(DEVKITPRO)/tools/bin/3dsxtool $< $@

$(OUT)/chimera-test.elf: main.c Makefile
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ main.c -lctru -lm

.PHONY: clean
clean:
	rm -f $(OUT)/chimera-test.elf $(OUT)/chimera-test.3dsx
