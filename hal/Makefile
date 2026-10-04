CC ?= gcc
AR ?= ar
CFLAGS ?= -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -O2
CPPFLAGS += -Iinclude -Isrc
LDFLAGS ?=

SRCS = src/hal.c \
       src/util.c \
       src/gpu.c \
       src/bluetooth.c \
       src/audio.c \
       src/backends/select.c \
       src/backends/linux/linux_util.c \
       src/backends/linux/gpu_linux.c \
       src/backends/linux/bluetooth_linux.c \
       src/backends/linux/audio_linux.c \
       src/backends/android/android_detect.c \
       src/backends/android/gpu_android.c \
       src/backends/android/bluetooth_android.c \
       src/backends/android/audio_android.c
OBJS = $(SRCS:.c=.o)
LIB = libhal.a
TESTBIN = tests/hal_smoke
TOOLBIN = tests/hal_info

all: $(LIB) $(TESTBIN) $(TOOLBIN)

$(LIB): $(OBJS)
	$(AR) rcs $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(TESTBIN): tests/hal_smoke.c $(LIB)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ tests/hal_smoke.c $(LIB) $(LDFLAGS)

$(TOOLBIN): tests/hal_info.c $(LIB)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ tests/hal_info.c $(LIB) $(LDFLAGS)

test: $(TESTBIN) $(TOOLBIN)
	./$(TESTBIN)
	./$(TOOLBIN)

clean:
	rm -f $(OBJS) $(LIB) $(TESTBIN) $(TOOLBIN)

.PHONY: all test clean
