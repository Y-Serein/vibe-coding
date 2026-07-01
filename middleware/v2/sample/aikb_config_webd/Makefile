SHELL = /bin/bash

TARGET := aikb_config_webd
SRCS := aikb_config_webd.c
OBJS := $(SRCS:.c=.o)

ifeq ($(BUILD_PATH),)
CC ?= gcc
CFLAGS ?= -O2
DEPFLAGS ?= -MMD
else
PARAM_FILE ?= ../../Makefile.param
include $(PARAM_FILE)
endif

EXTRA_CFLAGS := -std=c99 -Wall -Wextra -Wno-unused-parameter
EXTRA_LDLIBS :=

.PHONY: all clean

all: $(TARGET)

%.o: %.c
	$(CC) $(DEPFLAGS) $(CFLAGS) $(EXTRA_CFLAGS) -o $@ -c $<

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(ELFFLAGS) -o $@ $(OBJS) $(EXTRA_LDLIBS)

clean:
	rm -f $(TARGET) $(OBJS) $(SRCS:.c=.d)

-include $(SRCS:.c=.d)
