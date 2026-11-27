# Build an ATBM6441 SDK firmware with its lower MAC replaced by this shim
#
# Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# From an empty build directory:
#   make -f <this dir>/Makefile SDK=/path/to/sdk                 AT demo
#   make -f <this dir>/Makefile SDK=/path/to/sdk VARIANT=sdio050 SDIO demo
# or from anywhere with O=<build directory>.
#
# The SDK tree is copied to $(O)/$(VARIANT) and built there by its own
# Makefile, with lib/libwifi.a swapped for a copy that has the LMAC
# objects (drop.list) taken out and the shim's put in. Needs the SDK's
# nds32le-elf toolchain on PATH. Nothing is written next to this file.

SRC := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
O ?= .
OUT := $(abspath $(O))
ifeq ($(OUT),$(SRC))
$(error build out of tree: run make -f $(SRC)/Makefile from a build directory, or set O)
endif

VARIANT ?= iot050
SDK ?= $(error set SDK to the SDK's sdk/ directory)
CROSS ?= nds32le-elf-
BUILD := $(OUT)/$(VARIANT)

CC := $(CROSS)gcc
AR := $(CROSS)ar
CFLAGS := -Os -g -Wall -Werror -fno-builtin -fomit-frame-pointer -nostdlib \
	-fno-strict-aliasing -ffunction-sections -fdata-sections

# the SDIO build lays the LMAC's packet memory out differently
ifneq ($(filter sdio%,$(VARIANT)),)
CFLAGS += -DLMAC_QEMU_SDIO
endif

OBJS := $(BUILD)/shim/lmac_qemu.o $(BUILD)/shim/stubs.o $(BUILD)/shim/sdio.o

.PHONY: all clean sdk
all: $(BUILD)/build/iot.out

sdk: $(BUILD)/.copied

$(BUILD)/.copied:
	mkdir -p $(BUILD)
	cp -a $(SDK)/. $(BUILD)/
	cp $(BUILD)/lib/libwifi.a $(BUILD)/libwifi.orig.a
	touch $@

$(BUILD)/shim/%.o: $(SRC)/src/%.c $(SRC)/src/lmac_qemu.h $(BUILD)/.copied
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/lib/libwifi.a: $(BUILD)/libwifi.orig.a $(SRC)/drop.list $(OBJS)
	cp $< $@.tmp
	$(AR) d $@.tmp $$(grep -v '^#' $(SRC)/drop.list)
	$(AR) r $@.tmp $(OBJS)
	mv $@.tmp $@

# The SDK's last step packs OTA images with a Windows tool and fails here:
# iot.out and ap.bin are made by then.
$(BUILD)/build/iot.out: $(BUILD)/lib/libwifi.a
	rm -f $(BUILD)/WIFI.elf $@
	$(MAKE) -C $(BUILD) all CROSS_COMPILE=$(CROSS) > $(BUILD)/make.log 2>&1 || true
	test -f $@ || { tail -30 $(BUILD)/make.log; exit 1; }
	@grep 'undefined reference' $(BUILD)/make.log | head -20 || true
	$(CROSS)size $@

$(BUILD)/libwifi.orig.a: $(BUILD)/.copied

clean:
	rm -rf $(BUILD)
