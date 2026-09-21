# Native (Linux/macOS) host build of the MMBasic interpreter.
#
#   linux/mmbasic      headless stdio platform (LN-01)
#   linux/mmbasic-sdl  SDL2 backend           (LN-03+)
#
# Object files live under linux/build/ so the shared mmbasic/ tree is not
# polluted with host objects (the Circle build writes .o next to the sources).
#
# Override CC/CFLAGS as needed:
#   make -C linux CC=gcc CFLAGS="-O0 -g"
REPO   := $(abspath $(CURDIR)/..)
MMB    := $(REPO)/mmbasic
CONSOLE:= $(REPO)/console
BUILD  := $(CURDIR)/build
OUT_STDIO := $(CURDIR)/mmbasic
OUT_SDL   := $(CURDIR)/mmbasic-sdl

CC     ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -DMMB_PLATFORM_POSIX \
	  -I$(CURDIR) -I$(MMB)/include -I$(MMB)/third_party -I$(CONSOLE) \
	  -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function
LDLIBS += -lm

SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null)
HAVE_SDL   := $(if $(strip $(SDL_LIBS)),1,)

RAMDISK_C := $(MMB)/src/ramdisk_data.c
HELP_C    := $(MMB)/src/help_data.c
VERSION_H := $(CURDIR)/mmb_version.h

MMB_SRCS := $(filter-out $(RAMDISK_C) $(HELP_C),$(wildcard $(MMB)/src/*.c)) \
	    $(RAMDISK_C) $(HELP_C)
TP_SRCS  := $(MMB)/third_party/cJSON.c $(MMB)/third_party/hxcmod.c \
	    $(MMB)/third_party/picojpeg.c $(MMB)/third_party/upng.c
COMMON_SRCS := $(MMB_SRCS) $(TP_SRCS) $(CONSOLE)/net_rxbuf.c \
	       storage_posix.c net_none.c
STDIO_SRCS := platform_stdio.c main.c
SDL_SRCS   := platform_sdl.c sdl_video.c sdl_console.c main_sdl.c

COMMON_OBJS := $(addprefix $(BUILD)/,$(notdir $(patsubst %.c,%.o,$(COMMON_SRCS))))
STDIO_OBJS  := $(COMMON_OBJS) $(BUILD)/platform_stdio.o $(BUILD)/main.o
SDL_OBJS    := $(COMMON_OBJS) $(BUILD)/platform_sdl.o $(BUILD)/sdl_video.o \
	       $(BUILD)/sdl_console.o $(BUILD)/main_sdl.o

VPATH := $(MMB)/src:$(MMB)/third_party:$(CONSOLE):$(CURDIR)

all: $(OUT_STDIO) sdl

sdl: $(OUT_SDL)

ifeq ($(HAVE_SDL),)
$(OUT_SDL):
	@echo "SDL2 not found (pkg-config sdl2); skipping $@"
$(BUILD)/platform_sdl.o $(BUILD)/sdl_video.o $(BUILD)/sdl_console.o $(BUILD)/main_sdl.o: ;
else
$(OUT_SDL): $(SDL_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SDL_OBJS) $(LDLIBS) $(SDL_LIBS)
$(BUILD)/platform_sdl.o $(BUILD)/sdl_video.o $(BUILD)/sdl_console.o $(BUILD)/main_sdl.o: CFLAGS += $(SDL_CFLAGS)
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
	 $(BUILD)/platform_sdl.d $(BUILD)/sdl_video.d $(BUILD)/sdl_console.d \
	 $(BUILD)/main_sdl.d

clean:
	rm -rf $(BUILD) $(OUT_STDIO) $(OUT_SDL)

.PHONY: all sdl clean
