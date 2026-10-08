# SCDE - Solo Coding Desktop Environment
#
#   make              build scde + the X test helpers + the plugins
#   make debug        build scde-debug (-O0 -g) in build-debug/
#   make test         HAL tests, App API tests, then the Xvfb test suite
#   make run          run scde (needs a running X server / $DISPLAY)
#   make hal          build only the HAL static library
#   make plugins      build only the example/test plugin shared objects
#   make install      install into $(PREFIX), DESTDIR is honoured
#   make uninstall    remove the installed files
#   make clean        remove build artifacts (never sources or docs)
#
# No architecture specific flags (-march/-mcpu/...) are used: the project is
# compiled natively for the target (ARM64, x86_64, ...) with the system
# compiler.

CC        = gcc
AR       ?= ar

# always-on: language level + portability warnings
STD       := -std=c11 -D_POSIX_C_SOURCE=200809L
WARN      := -Wall -Wextra -Wpedantic
# optimisation; override with   make OPT=-O0   or just use  make debug
OPT       ?= -O2

CFLAGS    = $(STD) $(WARN) $(OPT) $(EXTRA_CFLAGS)
# public App API headers live in include/, the API itself in src/api + src/plugin
CPPFLAGS += -Iinclude
LDFLAGS   ?= $(EXTRA_LDFLAGS)
LDLIBS    ?= -lX11 -ldl

PREFIX    ?= /usr/local
BINDIR    ?= $(PREFIX)/bin
LIBDIR    ?= $(PREFIX)/lib
INCDIR    ?= $(PREFIX)/include
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
      src/panel.c src/launcher.c src/apps.c src/msgbox.c src/fm.c \
      src/desktop.c src/security_gui.c
OBJ = $(patsubst %.c,$(OBJDIR)/%.o,$(SRC))

# the App API + plugin system: a static library shared by scde and the tests
APISRC = src/api/common.c src/api/config.c src/api/event.c \
         src/api/notification.c src/api/window.c src/api/app.c \
         src/plugin/loader.c src/plugin/manager.c \
         src/security/core.c src/security/scanner.c src/security/policy.c \
         src/security/audit.c src/security/capability.c \
         src/security/sandbox.c src/security/bait.c \
         src/security/mirage.c src/security/resource.c
APIOBJ = $(patsubst %.c,$(OBJDIR)/%.o,$(APISRC))
APILIB = $(OBJDIR)/libscdeapp.a

DEP = $(OBJ:.o=.d) $(APIOBJ:.o=.d)

TESTBIN = tests/testclient tests/rootpixel tests/screenshot tests/api_test \
          tests/security_test

# plugin shared objects (needed by the API tests)
PLUGIN_DIR = $(OBJDIR)/plugins
EXAMPLE_SO  = $(PLUGIN_DIR)/libscde-example.so
BADVER_SO   = $(PLUGIN_DIR)/test-badver.so
FAILINIT_SO = $(PLUGIN_DIR)/test-failinit.so
PLUGINS     = $(EXAMPLE_SO) $(BADVER_SO) $(FAILINIT_SO)

all: $(BIN) $(TESTBIN)

$(BIN): $(OBJ) $(APILIB)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(APILIB) $(LDLIBS)

$(APILIB): $(APIOBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $(APIOBJ)

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

tests/%: tests/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

tests/security_test: tests/security_test.c src/util.c $(APILIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/security_test.c \
		src/util.c $(APILIB) $(LDLIBS)

tests/api_test: tests/api_test.c $(APILIB) $(PLUGINS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/api_test.c \
		$(APILIB) $(LDLIBS)

$(EXAMPLE_SO): plugins/example/example.c $(wildcard include/scde/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -shared -o $@ $<

$(BADVER_SO): tests/plugins/badver.c $(wildcard include/scde/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -shared -o $@ $<

$(FAILINIT_SO): tests/plugins/failinit.c $(wildcard include/scde/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -shared -o $@ $<

plugins: $(PLUGINS)

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
	@echo "== SCDE App API tests =="
	./tests/api_test $(EXAMPLE_SO) $(BADVER_SO) $(FAILINIT_SO)
	@echo "== SCDE security boundary tests =="
	./tests/security_test $(EXAMPLE_SO)
	@echo "== SCDE test suite (limit $(TEST_TIMEOUT)s) =="
	timeout -k 10 $(TEST_TIMEOUT) bash tests/run_tests.sh

install: $(BIN) $(APILIB)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
	install -d $(DESTDIR)$(SHAREDIR)
	install -m 644 config/scde-config.sample $(DESTDIR)$(SHAREDIR)/config.sample
	install -d $(DESTDIR)$(INCDIR)/scde
	install -m 644 include/scde/*.h $(DESTDIR)$(INCDIR)/scde/
	install -d $(DESTDIR)$(LIBDIR)
	install -m 644 $(APILIB) $(DESTDIR)$(LIBDIR)/libscdeapp.a

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)
	rm -f $(DESTDIR)$(SHAREDIR)/config.sample
	rm -rf $(DESTDIR)$(INCDIR)/scde
	rm -f $(DESTDIR)$(LIBDIR)/libscdeapp.a
	-rmdir $(DESTDIR)$(SHAREDIR)

clean:
	rm -rf $(OBJDIR) $(DEBUGDIR)
	rm -f src/*.o $(BIN) $(DEBUGBIN) $(TESTBIN)
	$(MAKE) -C $(HALDIR) clean

-include $(DEP)

.PHONY: all debug run hal plugins test install uninstall clean
