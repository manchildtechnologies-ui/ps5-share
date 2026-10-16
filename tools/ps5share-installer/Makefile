# PS5 Share Hub installer payload: writes the embedded title folder to
# /data/homebrew/PPSA99992. Usage:
#   make DIST=../../dist/PPSA99992 OUT=out [PS5_PAYLOAD_SDK=...]
#
# SPDX-License-Identifier: GPL-3.0-or-later

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

DIST ?= ../../dist/PPSA99992
OUT ?= out

CFLAGS := -Wall -Werror -O2
LDADD := -lSceNotification

all: $(OUT)/ps5-share-installer.elf

$(OUT)/table.c:
	python3 gen.py $(DIST) $(OUT)

$(OUT)/ps5-share-installer.elf: installer.c $(OUT)/table.c
	$(CC) $(CFLAGS) installer.c $(OUT)/table.c $(OUT)/data/*.o $(LDADD) -o $@

clean:
	rm -rf $(OUT)

.PHONY: all clean
