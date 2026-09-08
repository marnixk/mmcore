#
# Makefile for the raspberrypi-mmbasic bare-metal console (Circle app)
#
CIRCLEHOME = ../circle
MMB        = ../mmbasic

# Circle --qemu sets NO_SDHOST (no CYW4343x on raspi3b). Hardware images
# leave SDHOST on so Pi 3 Wi-Fi can share the board with the SD card.
-include $(CIRCLEHOME)/Config.mk
ifeq ($(findstring NO_SDHOST,$(DEFINE)),)
DEFINE += -DMMB_CIRCLE_WLAN
endif

OBJS	= main.o kernel.o platform.o storage.o wlan.o wlan_escan.o net.o audio.o \
	  $(MMB)/src/util.o \
	  $(MMB)/src/vars.o \
	  $(MMB)/src/expr.o \
	  $(MMB)/src/core.o \
	  $(MMB)/src/cmd_help.o \
	  $(MMB)/src/cmd_ihelp.o \
	  $(MMB)/src/cmd_option.o \
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
	  $(MMB)/src/cmd_lang.o \
	  $(MMB)/src/cmd_load.o \
	  $(MMB)/src/png.o \
	  $(MMB)/src/cmd_sprite.o \
	  $(MMB)/src/cmd_play.o \
	  $(MMB)/src/editor.o \
	  $(MMB)/src/assets.o \
	  $(MMB)/src/libc_shims.o \
	  $(MMB)/third_party/picojpeg.o \
	  $(MMB)/third_party/hxcmod.o \
	  $(MMB)/third_party/upng.o

LIBS	= $(CIRCLEHOME)/addon/fatfs/libfatfs.a \
	  $(CIRCLEHOME)/addon/SDCard/libsdcard.a \
	  $(CIRCLEHOME)/lib/sound/libsound.a \
	  $(CIRCLEHOME)/lib/usb/libusb.a \
	  $(CIRCLEHOME)/lib/input/libinput.a \
	  $(CIRCLEHOME)/lib/fs/libfs.a \
	  $(CIRCLEHOME)/lib/libcircle.a

ifeq ($(findstring MMB_CIRCLE_WLAN,$(DEFINE)),MMB_CIRCLE_WLAN)
LIBS := $(CIRCLEHOME)/addon/wlan/hostap/wpa_supplicant/libwpa_supplicant.a \
	$(CIRCLEHOME)/addon/wlan/libwlan.a \
	$(CIRCLEHOME)/lib/net/libnet.a \
	$(CIRCLEHOME)/lib/sched/libsched.a \
	$(LIBS)
endif

EXTRAINCLUDE = -I $(MMB)/include -I $(MMB)/third_party

# MMBasic objects live outside console/; drop them on `make clean` so a
# RASPPI switch (Pi 3 vs Pi 4 / 400) cannot relink stale -mcpu objects.
EXTRACLEAN = $(MMB)/src/*.o $(MMB)/src/*.d \
	     $(MMB)/third_party/*.o $(MMB)/third_party/*.d

include $(CIRCLEHOME)/Rules.mk

-include $(DEPS)
