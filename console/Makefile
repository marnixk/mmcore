#
# Makefile for the raspberrypi-mmbasic bare-metal console (Circle app)
#
CIRCLEHOME = ../circle
MMB        = ../mmbasic

# TCP/IP (libnet + libsched) is in every image so QEMU can drive Ethernet
# via usb-net. Circle --qemu sets NO_SDHOST (no CYW4343x on raspi3b);
# hardware images leave SDHOST on so Pi 3 Wi-Fi can share the SD card.
-include $(CIRCLEHOME)/Config.mk
DEFINE += -DMMB_CIRCLE_NET
ifeq ($(findstring NO_SDHOST,$(DEFINE)),)
DEFINE += -DMMB_CIRCLE_WLAN
endif

OBJS	= main.o kernel.o platform.o storage.o wlan.o wlan_escan.o eth.o net.o net_rxbuf.o audio.o \
	  $(MMB)/src/util.o \
	  $(MMB)/src/tok.o \
	  $(MMB)/src/vars.o \
	  $(MMB)/src/cmd_struct.o \
	  $(MMB)/src/cmd_json.o \
	  $(MMB)/src/expr.o \
	  $(MMB)/src/core.o \
	  $(MMB)/src/cmd_help.o \
	  $(MMB)/src/help_data.o \
	  $(MMB)/src/cmd_ihelp.o \
	  $(MMB)/src/cmd_option.o \
	  $(MMB)/src/tracecache.o \
	  $(MMB)/src/settings.o \
	  $(MMB)/src/gfx.o \
	  $(MMB)/src/gfx_cmm2.o \
	  $(MMB)/src/cmd_graphics.o \
	  $(MMB)/src/vfs.o \
	  $(MMB)/src/zip.o \
	  $(MMB)/src/cmd_package.o \
	  $(MMB)/src/cmd_files.o \
	  $(MMB)/src/cmd_files_ui.o \
	  $(MMB)/src/tui.o \
	  $(MMB)/src/cmd_connect.o \
	  $(MMB)/src/cmd_term.o \
	  $(MMB)/src/cmd_wordpad.o \
	  $(MMB)/src/cmd_credits.o \
	  $(MMB)/src/cmd_math.o \
	  $(MMB)/src/font_cp437_8x16.o \
	  $(MMB)/src/font_tnr_8x16.o \
	  $(MMB)/src/font_tnr_16x32.o \
	  $(MMB)/src/font_tnr_24x48.o \
	  $(MMB)/src/font_tnr_32x64.o \
	  $(MMB)/src/cmd_lang.o \
	  $(MMB)/src/cmd_array.o \
	  $(MMB)/src/cmd_load.o \
	  $(MMB)/src/png.o \
	  $(MMB)/src/blit16.o \
	  $(MMB)/src/blit_a64.o \
	  $(MMB)/src/cmd_sprite.o \
	  $(MMB)/src/cmd_play.o \
	  $(MMB)/src/editor.o \
	  $(MMB)/src/ramdisk.o \
	  $(MMB)/src/ramdisk_data.o \
	  $(MMB)/src/libc_shims.o \
	  $(MMB)/third_party/picojpeg.o \
	  $(MMB)/third_party/hxcmod.o \
	  $(MMB)/third_party/upng.o \
	  $(MMB)/third_party/cJSON.o

LIBS	= $(CIRCLEHOME)/lib/net/libnet.a \
	  $(CIRCLEHOME)/lib/sched/libsched.a \
	  $(CIRCLEHOME)/addon/fatfs/libfatfs.a \
	  $(CIRCLEHOME)/addon/SDCard/libsdcard.a \
	  $(CIRCLEHOME)/lib/sound/libsound.a \
	  $(CIRCLEHOME)/lib/usb/libusb.a \
	  $(CIRCLEHOME)/lib/input/libinput.a \
	  $(CIRCLEHOME)/lib/fs/libfs.a \
	  $(CIRCLEHOME)/lib/libcircle.a

ifeq ($(findstring MMB_CIRCLE_WLAN,$(DEFINE)),MMB_CIRCLE_WLAN)
LIBS := $(CIRCLEHOME)/addon/wlan/hostap/wpa_supplicant/libwpa_supplicant.a \
	$(CIRCLEHOME)/addon/wlan/libwlan.a \
	$(LIBS)
endif

EXTRAINCLUDE = -I . -I $(MMB)/include -I $(MMB)/third_party

MMB_VERSION ?= $(shell git -C $(MMB)/.. describe --tags --always 2>/dev/null)
ifeq ($(strip $(MMB_VERSION)),)
MMB_VERSION := dev
endif

# MMBasic objects live outside console/; drop them on `make clean` so a
# RASPPI switch (Pi 3 vs Pi 4 / 400) cannot relink stale -mcpu objects.
HELP_DOCS = $(wildcard $(MMB)/../docs/help/*.txt)

# Versioned ramdisk/ tree embedded into the kernel. RAMDISK_EXCLUDE is a
# comma-separated list of top-level folders to omit (release builds drop
# `tests`). The mode stamp forces a regenerate when the option changes.
RAMDISK_EXCLUDE ?=
RAMDISK_FILES = $(shell find $(MMB)/../ramdisk -type f 2>/dev/null)
RAMDISK_STAMP = $(MMB)/src/ramdisk_data.stamp

EXTRACLEAN = $(MMB)/src/*.o $(MMB)/src/*.d \
	     $(MMB)/src/help_data.c \
	     $(MMB)/src/ramdisk_data.c $(MMB)/src/ramdisk_data.stamp \
	     $(MMB)/third_party/*.o $(MMB)/third_party/*.d \
	     mmb_version.h

include $(CIRCLEHOME)/Rules.mk

mmb_version.h: FORCE
	MMB_VERSION="$(MMB_VERSION)" bash $(MMB)/../scripts/gen-mmb-version.sh $(CURDIR)/mmb_version.h

$(MMB)/src/help_data.c: $(MMB)/../scripts/gen_help.py $(HELP_DOCS)
	python3 $(MMB)/../scripts/gen_help.py $(MMB)/src/help_data.c

$(RAMDISK_STAMP): FORCE
	@printf '%s' "$(RAMDISK_EXCLUDE)" | cmp -s - $@ || printf '%s' "$(RAMDISK_EXCLUDE)" > $@

$(MMB)/src/ramdisk_data.c: $(MMB)/../scripts/gen_ramdisk.py $(RAMDISK_FILES) $(RAMDISK_STAMP)
	python3 $(MMB)/../scripts/gen_ramdisk.py --out $@ --exclude "$(RAMDISK_EXCLUDE)"

$(MMB)/src/util.o $(MMB)/src/cmd_credits.o: mmb_version.h

.PHONY: FORCE
FORCE:

-include $(DEPS)
