# SCDE - Solo Coding Desktop Environment
#
#   make              build scde + the X test helpers
#   make debug        build scde-debug (-O0 -g) in build-debug/
#   make test         build and run the HAL tests and the Xvfb test suite
#   make run          run scde (needs a running X server / $DISPLAY)
#   make hal          build only the HAL static library
#   make install      install into $(PREFIX), DESTDIR is honoured
#   make uninstall    remove the installed files
#   make clean        remove build artifacts (never sources or docs)
#
# No architecture specific flags (-march/-mcpu/...) are used: the project is
# compiled natively for the target (ARM64, x86_64, ...) with the system
# compiler.

CC        = gcc

# always-on: language level + portability warnings
STD       := -std=c11 -D_POSIX_C_SOURCE=200809L
WARN      := -Wall -Wextra -Wpedantic
# optimisation; override with   make OPT=-O0   or just use  make debug
OPT       ?= -O2

CFLAGS    = $(STD) $(WARN) $(OPT) $(EXTRA_CFLAGS)
CPPFLAGS  ?=
LDFLAGS   ?= $(EXTRA_LDFLAGS)
LDLIBS    ?= -lX11

PREFIX    ?= /usr/local
BINDIR    ?= $(PREFIX)/bin
SHAREDIR  ?= $(PREFIX)/share/scde
DESTDIR   ?=

BIN       = scde
DEBUGBIN  = scde-debug
OBJDIR    = build
DEBUGDIR  = build-debug

HALDIR    = hal
# hard limit for the Xvfb suite so `make test` can never hang
TEST_TIMEOUT ?= 300

SRC = src/main.c src/util.c src/config.c src/wm.c src/input.c \
      src/panel.c src/launcher.c src/apps.c src/msgbox.c src/fm.c
OBJ = $(patsubst %.c,$(OBJDIR)/%.o,$(SRC))
DEP = $(OBJ:.o=.d)

TESTBIN = tests/testclient tests/rootpixel tests/screenshot

all: $(BIN) $(TESTBIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

tests/%: tests/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

# a separate object directory keeps the release build intact
debug:
	@$(MAKE) BIN=$(DEBUGBIN) OBJDIR=$(DEBUGDIR) OPT="-O0 -g" $(DEBUGBIN)

run: $(BIN)
	./$(BIN) $(ARGS)

hal:
	$(MAKE) -C $(HALDIR) all

test: $(BIN) $(TESTBIN) hal
	@echo "== HAL tests =="
	$(MAKE) -C $(HALDIR) test
	@echo "== SCDE test suite (limit $(TEST_TIMEOUT)s) =="
	timeout -k 10 $(TEST_TIMEOUT) bash tests/run_tests.sh

install: $(BIN)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
	install -d $(DESTDIR)$(SHAREDIR)
	install -m 644 config/scde-config.sample $(DESTDIR)$(SHAREDIR)/config.sample

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)
	rm -f $(DESTDIR)$(SHAREDIR)/config.sample
	-rmdir $(DESTDIR)$(SHAREDIR)

clean:
	rm -rf $(OBJDIR) $(DEBUGDIR)
	rm -f src/*.o $(BIN) $(DEBUGBIN) $(TESTBIN)
	$(MAKE) -C $(HALDIR) clean

-include $(DEP)

.PHONY: all debug run hal test install uninstall clean
