# Native (Linux/macOS) host build of the MMBasic interpreter.
#
# LN-01: headless stdio platform, output linux/mmbasic.
# Object files live under linux/build/ so the shared mmbasic/ tree is not
# polluted with host objects (the Circle build writes .o next to the sources).
#
# Override CC/CFLAGS as needed:
#   make -C linux CC=gcc CFLAGS="-O0 -g"
REPO   := $(abspath $(CURDIR)/..)
MMB    := $(REPO)/mmbasic
CONSOLE:= $(REPO)/console
BUILD  := $(CURDIR)/build
OUT    := $(CURDIR)/mmbasic

CC     ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -DMMB_PLATFORM_POSIX \
	  -I$(CURDIR) -I$(MMB)/include -I$(MMB)/third_party -I$(CONSOLE) \
	  -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function
LDLIBS += -lm

RAMDISK_C := $(MMB)/src/ramdisk_data.c
HELP_C    := $(MMB)/src/help_data.c
VERSION_H := $(CURDIR)/mmb_version.h

MMB_SRCS := $(filter-out $(RAMDISK_C) $(HELP_C),$(wildcard $(MMB)/src/*.c)) \
	    $(RAMDISK_C) $(HELP_C)
TP_SRCS  := $(MMB)/third_party/cJSON.c $(MMB)/third_party/hxcmod.c \
	    $(MMB)/third_party/picojpeg.c $(MMB)/third_party/upng.c
NATIVE_SRCS := $(CONSOLE)/net_rxbuf.c \
	       platform_stdio.c storage_none.c net_none.c main.c

SRCS := $(MMB_SRCS) $(TP_SRCS) $(NATIVE_SRCS)
OBJS := $(addprefix $(BUILD)/,$(notdir $(patsubst %.c,%.o,$(SRCS))))

VPATH := $(MMB)/src:$(MMB)/third_party:$(CONSOLE):$(CURDIR)

all: $(OUT)

$(OUT): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

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

-include $(OBJS:.o=.d)

clean:
	rm -rf $(BUILD) $(OUT)

.PHONY: all clean
