# Native (Linux/macOS/Windows) host build of the MMBasic interpreter.
#
#   native/mmbasic      headless stdio platform (LN-01)
#   native/mmbasic-sdl  SDL2 backend           (LN-03+)
#
# Object files live under native/build/ (Windows objects under
# native/build/win) so the shared mmbasic/ tree is not polluted with host
# objects (the Circle build writes .o next to the sources).
#
# Override CC/CFLAGS as needed:
#   make -C native CC=gcc CFLAGS="-O0 -g"
#
# Windows: detected from the host (MinGW/MSYS2) or forced with
#   make -C native TARGET_WINDOWS=1 CC=x86_64-w64-mingw32-gcc
# It swaps the POSIX sockets backend for Winsock and links -lws2_32, and turns
# on the SDL_MAIN_HANDLED path so our own main() is the entry point.
REPO   := $(abspath $(CURDIR)/..)
MMB    := $(REPO)/mmbasic
CONSOLE:= $(REPO)/console

UNAME_S := $(shell uname -s 2>/dev/null)
ifeq ($(OS),Windows_NT)
WIN_BUILD := 1
else ifneq (,$(findstring MINGW,$(UNAME_S)))
WIN_BUILD := 1
else ifneq (,$(findstring MSYS,$(UNAME_S)))
WIN_BUILD := 1
endif
ifneq ($(TARGET_WINDOWS),)
WIN_BUILD := 1
endif

# Keep Windows objects apart from the POSIX ones so alternating native and
# cross builds never link a mixed object set.
BUILD  := $(CURDIR)/build$(if $(WIN_BUILD),/win,)

CC     ?= cc
CFLAGS ?= -O2 -g

EXE :=
NET_SRC := net_posix.c
ifneq ($(WIN_BUILD),)
EXE := .exe
NET_SRC := net_win.c
CFLAGS += -DSDL_MAIN_HANDLED -I$(CURDIR)/win
LDLIBS += -lws2_32 -lwinmm -limm32 -lole32 -loleaut32 -luuid -lversion \
	  -lsetupapi -luser32 -lgdi32 -lshell32
endif

OUT_STDIO := $(CURDIR)/mmbasic$(EXE)
OUT_SDL   := $(CURDIR)/mmbasic-sdl$(EXE)

CFLAGS += -DMMB_PLATFORM_POSIX \
	  -I$(CURDIR) -I$(MMB)/include -I$(MMB)/third_party -I$(CONSOLE) \
	  -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function
LDLIBS += -lm

SDL_CFLAGS ?= $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS   ?= $(shell pkg-config --libs sdl2 2>/dev/null)
# MinGW's sdl2.pc hardcodes SDL's WinMain shim; we supply main() ourselves.
SDL_CFLAGS := $(filter-out -Dmain=SDL_main,$(SDL_CFLAGS))
HAVE_SDL   := $(if $(strip $(SDL_LIBS)),1,)

RAMDISK_C := $(MMB)/src/ramdisk_data.c
HELP_C    := $(MMB)/src/help_data.c
VERSION_H := $(CURDIR)/mmb_version.h

MMB_SRCS := $(filter-out $(RAMDISK_C) $(HELP_C),$(wildcard $(MMB)/src/*.c)) \
	    $(RAMDISK_C) $(HELP_C)
TP_SRCS  := $(MMB)/third_party/cJSON.c $(MMB)/third_party/hxcmod.c \
	    $(MMB)/third_party/picojpeg.c $(MMB)/third_party/upng.c
COMMON_SRCS := $(MMB_SRCS) $(TP_SRCS) $(CONSOLE)/net_rxbuf.c \
	       storage_posix.c $(NET_SRC) cli.c
STDIO_SRCS := platform_stdio.c main.c
SDL_SRCS   := platform_sdl.c sdl_video.c sdl_scale.c sdl_console.c sdl_tui.c \
	      sdl_input.c sdl_audio.c main_sdl.c

COMMON_OBJS := $(addprefix $(BUILD)/,$(notdir $(patsubst %.c,%.o,$(COMMON_SRCS))))
STDIO_OBJS  := $(COMMON_OBJS) $(BUILD)/platform_stdio.o $(BUILD)/main.o
SDL_ONLY_OBJS := $(addprefix $(BUILD)/,$(notdir $(patsubst %.c,%.o,$(SDL_SRCS))))
SDL_OBJS    := $(COMMON_OBJS) $(SDL_ONLY_OBJS)

VPATH := $(MMB)/src:$(MMB)/third_party:$(CONSOLE):$(CURDIR)

all: $(OUT_STDIO) sdl

stdio: $(OUT_STDIO)

sdl: $(OUT_SDL)

ifeq ($(HAVE_SDL),)
$(OUT_SDL):
	@echo "SDL2 not found (pkg-config sdl2); skipping $@"
$(SDL_ONLY_OBJS): ;
else
$(OUT_SDL): $(SDL_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SDL_OBJS) $(LDLIBS) $(SDL_LIBS)
$(SDL_ONLY_OBJS): CFLAGS += $(SDL_CFLAGS)
endif

$(OUT_STDIO): $(STDIO_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(STDIO_OBJS) $(LDLIBS)

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/ramdisk_data.o: $(RAMDISK_C)
$(BUILD)/help_data.o: $(HELP_C)
$(BUILD)/util.o $(BUILD)/cmd_credits.o: $(VERSION_H)

$(HELP_C): $(REPO)/scripts/gen_help.py $(wildcard $(REPO)/docs/help/*.txt)
	python3 $(REPO)/scripts/gen_help.py $@

$(RAMDISK_C): $(REPO)/scripts/gen_ramdisk.py $(shell find $(REPO)/ramdisk -type f 2>/dev/null)
	python3 $(REPO)/scripts/gen_ramdisk.py --out $@

$(VERSION_H):
	MMB_VERSION="$(shell git -C $(REPO) describe --tags --always 2>/dev/null)" \
		bash $(REPO)/scripts/gen-mmb-version.sh $@

-include $(COMMON_OBJS:.o=.d) $(BUILD)/platform_stdio.d $(BUILD)/main.d \
	 $(SDL_ONLY_OBJS:.o=.d)

clean:
	rm -rf $(CURDIR)/build
	rm -f $(CURDIR)/mmbasic $(CURDIR)/mmbasic.exe \
	      $(CURDIR)/mmbasic-sdl $(CURDIR)/mmbasic-sdl.exe

.PHONY: all stdio sdl clean
