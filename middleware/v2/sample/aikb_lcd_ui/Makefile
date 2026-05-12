SHELL = /bin/bash

TARGET := aikb_lcd_ui
SRCS := aikb_lcd_ui.c kitty_graphics.c
OBJS := $(SRCS:.c=.o)

ifeq ($(BUILD_PATH),)
CC ?= gcc
CFLAGS ?= -O2
DEPFLAGS ?= -MMD
else
PARAM_FILE ?= ../../Makefile.param
include $(PARAM_FILE)
endif

EXTRA_CFLAGS := -std=c99 -Wall -Wextra -Wno-unused-parameter -I../../3rdparty/stb
EXTRA_LDLIBS :=
FT_SYSROOT ?= ../../../../buildroot/output/host/riscv64-buildroot-linux-musl/sysroot

ifneq ($(findstring riscv,$(notdir $(CC))),)
ifneq ($(wildcard $(FT_SYSROOT)/usr/include/freetype2/ft2build.h),)
EXTRA_CFLAGS += --sysroot=$(FT_SYSROOT) -I$(FT_SYSROOT)/usr/include/freetype2 -DAIKB_USE_FREETYPE=1
EXTRA_LDLIBS += --sysroot=$(FT_SYSROOT) -lfreetype
else
EXTRA_CFLAGS += -DAIKB_USE_FREETYPE=0
endif
else
FT_CFLAGS := $(shell pkg-config --cflags freetype2 2>/dev/null)
FT_LIBS := $(shell pkg-config --libs freetype2 2>/dev/null)
ifneq ($(strip $(FT_CFLAGS)$(FT_LIBS)),)
EXTRA_CFLAGS += $(FT_CFLAGS) -DAIKB_USE_FREETYPE=1
EXTRA_LDLIBS += $(FT_LIBS)
else
EXTRA_CFLAGS += -DAIKB_USE_FREETYPE=0
endif
endif

.PHONY: all clean

all: $(TARGET)

%.o: %.c
	$(CC) $(DEPFLAGS) $(CFLAGS) $(EXTRA_CFLAGS) -o $@ -c $<

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(ELFFLAGS) -o $@ $(OBJS) $(EXTRA_LDLIBS)

clean:
	rm -f $(TARGET) $(OBJS) $(SRCS:.c=.d)

-include $(SRCS:.c=.d)
