#
# Makefile for the raspberrypi-mmbasic bare-metal console (Circle app)
#
CIRCLEHOME = ../circle
MMB        = ../mmbasic

OBJS	= main.o kernel.o platform.o \
	  $(MMB)/src/util.o \
	  $(MMB)/src/vars.o \
	  $(MMB)/src/expr.o \
	  $(MMB)/src/core.o \
	  $(MMB)/src/cmd_option.o \
	  $(MMB)/src/gfx.o \
	  $(MMB)/src/cmd_graphics.o \
	  $(MMB)/src/vfs.o \
	  $(MMB)/src/cmd_files.o \
	  $(MMB)/src/cmd_load.o \
	  $(MMB)/src/png.o \
	  $(MMB)/src/cmd_play.o \
	  $(MMB)/src/editor.o \
	  $(MMB)/src/assets.o \
	  $(MMB)/src/libc_shims.o \
	  $(MMB)/third_party/picojpeg.o \
	  $(MMB)/third_party/hxcmod.o

LIBS = $(CIRCLEHOME)/lib/libcircle.a

EXTRAINCLUDE = -I $(MMB)/include -I $(MMB)/third_party

include $(CIRCLEHOME)/Rules.mk

-include $(DEPS)
