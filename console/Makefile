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

# Vendored libsmb2 (#1128/#1132) for OPTION NETWORK DRIVE on Z:. The shared
# backend (mmbasic/src/netfs_smb2.c) talks to the BSD socket surface in
# console/smb2_sock.cpp, which sits on Circle CSocket. lib/sync.c is dropped
# because it blocks in poll(1000) with no break hook; the backend only uses
# the async API.
SMB2      = $(MMB)/third_party/libsmb2
SMB2_SRCS = $(filter-out $(SMB2)/lib/sync.c,$(wildcard $(SMB2)/lib/*.c))
SMB2_OBJS = $(SMB2_SRCS:.c=.o)

# libsmb2's md5.c defines unprefixed MD5Init/MD5Update/MD5Final. Hardware
# images (NO_SDHOST unset -> MMB_CIRCLE_WLAN) also link the hostap
# wpa_supplicant archive, which defines the same three, so the link fails with
# multiple definitions. Namespace libsmb2's copies for every vendored source so
# its own callers bind to its implementation, not hostap's. QEMU images that
# skip hostap are unaffected but get the same names for consistency.
$(SMB2_OBJS): DEFINE += -DMD5Init=mmb_smb2_MD5Init \
			 -DMD5Update=mmb_smb2_MD5Update \
			 -DMD5Final=mmb_smb2_MD5Final

OBJS	= main.o kernel.o platform.o storage.o wlan.o wlan_escan.o eth.o net.o net_rxbuf.o audio.o \
	  smb2_sock.o \
	  $(MMB)/src/netfs_smb2.o \
	  $(MMB)/src/netdrive_unc.o \
	  $(SMB2_OBJS) \
	  $(MMB)/src/util.o \
	  $(MMB)/src/frontend.o \
	  $(MMB)/src/session.o \
	  $(MMB)/src/yield.o \
	  $(MMB)/src/tok.o \
	  $(MMB)/src/vars.o \
	  $(MMB)/src/cmd_struct.o \
	  $(MMB)/src/cmd_json.o \
	  $(MMB)/src/expr.o \
	  $(MMB)/src/core.o \
	  $(MMB)/src/exec_frame.o \
	  $(MMB)/src/cmd_help.o \
	  $(MMB)/src/help_data.o \
	  $(MMB)/src/cmd_ihelp.o \
	  $(MMB)/src/cmd_option.o \
	  $(MMB)/src/tracecache.o \
	  $(MMB)/src/settings.o \
	  $(MMB)/src/battery.o \
	  $(MMB)/src/gfx.o \
	  $(MMB)/src/frame_prof.o \
	  $(MMB)/src/stmt_prof.o \
	  $(MMB)/src/gfx_cmm2.o \
	  $(MMB)/src/cmd_graphics.o \
	  $(MMB)/src/vfs.o \
	  $(MMB)/src/zip.o \
	  $(MMB)/src/inflate.o \
	  $(MMB)/src/cmd_package.o \
	  $(MMB)/src/cmd_apppath.o \
	  $(MMB)/src/cmd_settings.o \
	  $(MMB)/src/cmd_files.o \
	  $(MMB)/src/cmd_files_ui.o \
	  $(MMB)/src/mmb_tdf.o \
	  $(MMB)/src/cmd_tdf.o \
	  $(MMB)/src/cmd_ftp.o \
	  $(MMB)/src/tui.o \
	  $(MMB)/src/cmd_connect.o \
	  $(MMB)/src/ntp.o \
	  $(MMB)/src/clipboard.o \
	  $(MMB)/src/cmd_term.o \
	  $(MMB)/src/zmodem.o \
	  $(MMB)/src/cmd_wordpad.o \
	  $(MMB)/src/cmd_credits.o \
	  $(MMB)/src/cmd_afk.o \
	  $(MMB)/src/cmd_math.o \
	  $(MMB)/src/font_cp437_8x16.o \
	  $(MMB)/src/font_cp437_8x14.o \
	  $(MMB)/src/font_tnr_8x16.o \
	  $(MMB)/src/font_tnr_16x32.o \
	  $(MMB)/src/font_tnr_24x48.o \
	  $(MMB)/src/font_tnr_32x64.o \
	  $(MMB)/src/cmd_lang.o \
	  $(MMB)/src/cmd_array.o \
	  $(MMB)/src/cmd_load.o \
	  $(MMB)/src/png.o \
	  $(MMB)/src/pcx.o \
	  $(MMB)/src/blit16.o \
	  $(MMB)/src/blit_a64.o \
	  $(MMB)/src/cmd_sprite.o \
	  $(MMB)/src/cmd_paint.o \
	  $(MMB)/src/paint_palette.o \
	  $(MMB)/src/paint_tools.o \
	  $(MMB)/src/paint_undo.o \
	  $(MMB)/src/paint_menus.o \
	  $(MMB)/src/paint_cursors.o \
	  $(MMB)/src/paint_file.o \
	  $(MMB)/src/paint_text.o \
	  $(MMB)/src/paint_select.o \
	  $(MMB)/src/paint_cursor_art.o \
	  $(MMB)/src/cmd_mouse.o \
	  $(MMB)/src/mouse_cursor.o \
	  $(MMB)/src/cmd_play.o \
	  $(MMB)/src/cmd_juke.o \
	  $(MMB)/src/editor.o \
	  $(MMB)/src/ramdisk.o \
	  $(MMB)/src/ramdisk_data.o \
	  $(MMB)/src/libc_shims.o \
	  $(MMB)/third_party/picojpeg.o \
	  $(MMB)/third_party/hxcmod.o \
	  $(MMB)/third_party/ibxm.o \
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

EXTRAINCLUDE = -I . -I $(MMB)/include -I $(MMB)/third_party \
	       -I $(SMB2)/include/circle -I $(SMB2)/include \
	       -I $(SMB2)/include/smb2 -I $(SMB2)/lib

# MMB_HAVE_SMB2 enables the shared Z: backend (netfs_smb2.c) and the OPTION
# NETWORK DRIVE parser. HAVE_CONFIG_H picks up include/circle/config.h (the
# directory is first on the include path above). _U_ silences libsmb2's
# intentionally unused iovectors the same way native/Makefile does.
DEFINE += -DMMB_HAVE_SMB2=1 -DHAVE_CONFIG_H -D_U_='__attribute__((unused))'

MMB_VERSION ?= $(shell git -C $(MMB)/.. describe --tags --always 2>/dev/null)
ifeq ($(strip $(MMB_VERSION)),)
MMB_VERSION := dev
endif

# MMBasic objects live outside console/; drop them on `make clean` so a
# RASPPI switch (Pi 3 vs Pi 4 / 400) cannot relink stale -mcpu objects.
HELP_DOCS = $(wildcard $(MMB)/../docs/help/*.txt)

# Versioned ramdisk/ tree embedded into the kernel. RAMDISK_EXCLUDE is a
# comma-separated list of top-level folders to omit (release builds drop
# `tests`). The stamp records the sorted list of embedded files, so a removed
# or renamed file (which only drops out of RAMDISK_FILES) still changes the
# stamp and forces a regenerate.
RAMDISK_EXCLUDE ?=
RAMDISK_FILES = $(shell find $(MMB)/../ramdisk -type f 2>/dev/null)
RAMDISK_STAMP = $(MMB)/src/ramdisk_data.stamp

EXTRACLEAN = $(MMB)/src/*.o $(MMB)/src/*.d \
	     $(MMB)/src/help_data.c \
	     $(MMB)/src/ramdisk_data.c $(MMB)/src/ramdisk_data.stamp \
	     $(MMB)/third_party/*.o $(MMB)/third_party/*.d \
	     $(SMB2)/lib/*.o $(SMB2)/lib/*.d \
	     mmb_version.h

include $(CIRCLEHOME)/Rules.mk

mmb_version.h: FORCE
	MMB_VERSION="$(MMB_VERSION)" bash $(MMB)/../scripts/gen-mmb-version.sh $(CURDIR)/mmb_version.h

$(MMB)/src/help_data.c: $(MMB)/../scripts/gen_help.py $(HELP_DOCS)
	python3 $(MMB)/../scripts/gen_help.py $(MMB)/src/help_data.c

$(RAMDISK_STAMP): FORCE
	python3 $(MMB)/../scripts/gen_ramdisk.py --stamp-out $@ --exclude "$(RAMDISK_EXCLUDE)"

# #882: ramdisk_data.c is shared with native/, which embeds the whole tree
# (no exclude) while a release console build embeds exclude=tests. Each build
# keeps its own stamp, so a build could otherwise see the other mode's file as
# up to date and link the wrong table. FORCE the generator every time; it
# rewrites only when the rendered text (including the embedded mode) differs.
$(MMB)/src/ramdisk_data.c: FORCE $(MMB)/../scripts/gen_ramdisk.py $(RAMDISK_FILES) $(RAMDISK_STAMP)
	python3 $(MMB)/../scripts/gen_ramdisk.py --out $@ --exclude "$(RAMDISK_EXCLUDE)"

$(MMB)/src/util.o $(MMB)/src/cmd_credits.o $(MMB)/src/expr.o: mmb_version.h

.PHONY: FORCE
FORCE:

-include $(DEPS)
