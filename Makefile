# fbmq — File-Based Message Queue
# Requires: GCC/Clang, GNU Make, Linux or macOS

# ── Version ──

VERSION_MAJOR := 1
VERSION_MINOR := 0
VERSION_PATCH := 0
VERSION       := $(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_PATCH)

# ── Install paths ──

PREFIX       ?= /usr/local
BINDIR       ?= $(PREFIX)/bin
LIBDIR       ?= $(PREFIX)/lib
INCLUDEDIR   ?= $(PREFIX)/include
MANDIR       ?= $(PREFIX)/share/man
PKGCONFIGDIR ?= $(LIBDIR)/pkgconfig

# ── Toolchain ──

CC       ?= gcc
AR       ?= ar
CFLAGS   ?= -O2 -Wall -Wextra -Wpedantic -std=c11
CPPFLAGS += -D_GNU_SOURCE -Iinclude -Isrc
LDFLAGS  ?=

# ── Link mode for CLI binary (static or shared) ──

LINK_MODE ?= static

# ── Platform detection ──

UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
  SHARED_EXT     := dylib
  SHARED_LIB     := libfbmq.$(VERSION).$(SHARED_EXT)
  SHARED_SONAME  := libfbmq.$(VERSION_MAJOR).$(SHARED_EXT)
  SHARED_LINKER  := libfbmq.$(SHARED_EXT)
  SHARED_FLAGS   := -dynamiclib -install_name $(LIBDIR)/$(SHARED_SONAME) \
                    -current_version $(VERSION) -compatibility_version $(VERSION_MAJOR).0.0
else
  SHARED_EXT     := so
  SHARED_LIB     := libfbmq.$(SHARED_EXT).$(VERSION)
  SHARED_SONAME  := libfbmq.$(SHARED_EXT).$(VERSION_MAJOR)
  SHARED_LINKER  := libfbmq.$(SHARED_EXT)
  SHARED_FLAGS   := -shared -Wl,-soname,$(SHARED_SONAME)
endif

# ── Sources ──

LIB_SRCS := src/fbmq.c src/md5.c
CLI_SRCS := src/fbmq_main.c

# ── Object directories ──

STATIC_OBJS := $(patsubst src/%.c,build/static/%.o,$(LIB_SRCS))
SHARED_OBJS := $(patsubst src/%.c,build/shared/%.o,$(LIB_SRCS))
CLI_OBJS    := $(patsubst src/%.c,build/static/%.o,$(CLI_SRCS))

# ── Targets ──

BIN        := fbmq
STATIC_LIB := libfbmq.a

.PHONY: all libs clean install install-bin install-lib uninstall test

all: $(BIN) libs

# ── CLI binary ──

ifeq ($(LINK_MODE),shared)
$(BIN): $(CLI_OBJS) $(SHARED_LIB)
	$(CC) $(LDFLAGS) -o $@ $(CLI_OBJS) -L. -lfbmq -Wl,-rpath,$(LIBDIR)
else
$(BIN): $(CLI_OBJS) $(STATIC_LIB)
	$(CC) $(LDFLAGS) -o $@ $(CLI_OBJS) $(STATIC_LIB)
endif

# ── Libraries ──

libs: $(STATIC_LIB) $(SHARED_LIB) fbmq.pc

$(STATIC_LIB): $(STATIC_OBJS)
	$(AR) rcs $@ $(STATIC_OBJS)

$(SHARED_LIB): $(SHARED_OBJS)
	$(CC) $(SHARED_FLAGS) $(LDFLAGS) -o $@ $(SHARED_OBJS)
	ln -sf $(SHARED_LIB) $(SHARED_SONAME)
	ln -sf $(SHARED_LIB) $(SHARED_LINKER)

# ── Object compilation ──

build/static/%.o: src/%.c include/fbmq.h src/md5.h | build/static
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

build/shared/%.o: src/%.c include/fbmq.h src/md5.h | build/shared
	$(CC) $(CFLAGS) $(CPPFLAGS) -fPIC -DFBMQ_SHARED_BUILD -fvisibility=hidden -c -o $@ $<

build/static build/shared:
	mkdir -p $@

# ── pkg-config ──

fbmq.pc: Makefile
	@printf '%s\n' \
		'prefix=$(PREFIX)' \
		'libdir=$${prefix}/lib' \
		'includedir=$${prefix}/include' \
		'' \
		'Name: fbmq' \
		'Description: File-Based Message Queue — lock-free, Markdown-native' \
		'Version: $(VERSION)' \
		'Cflags: -I$${includedir}' \
		'Libs: -L$${libdir} -lfbmq' \
		> $@

# ── Clean ──

clean:
	rm -rf build $(BIN) $(STATIC_LIB) $(SHARED_LIB) $(SHARED_SONAME) $(SHARED_LINKER) fbmq.pc

# ── Install ──

install: install-bin install-lib

install-bin: $(BIN)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
	install -d $(DESTDIR)$(MANDIR)/man1
	install -m 644 man/man1/fbmq.1 $(DESTDIR)$(MANDIR)/man1/fbmq.1
	install -d $(DESTDIR)$(MANDIR)/man5
	install -m 644 man/man5/fbmq-message.5 $(DESTDIR)$(MANDIR)/man5/fbmq-message.5
	install -d $(DESTDIR)$(MANDIR)/man7
	install -m 644 man/man7/fbmq-design.7 $(DESTDIR)$(MANDIR)/man7/fbmq-design.7
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 scripts/fbmq-reaper $(DESTDIR)$(BINDIR)/fbmq-reaper
	install -m 755 scripts/fbmq-worker $(DESTDIR)$(BINDIR)/fbmq-worker
	install -d $(DESTDIR)$(MANDIR)/man1
	install -m 644 man/man1/fbmq-worker.1 $(DESTDIR)$(MANDIR)/man1/fbmq-worker.1

install-lib: libs
	install -d $(DESTDIR)$(LIBDIR)
	install -m 644 $(STATIC_LIB) $(DESTDIR)$(LIBDIR)/$(STATIC_LIB)
	install -m 755 $(SHARED_LIB) $(DESTDIR)$(LIBDIR)/$(SHARED_LIB)
	ln -sf $(SHARED_LIB) $(DESTDIR)$(LIBDIR)/$(SHARED_SONAME)
	ln -sf $(SHARED_LIB) $(DESTDIR)$(LIBDIR)/$(SHARED_LINKER)
	install -d $(DESTDIR)$(INCLUDEDIR)
	install -m 644 include/fbmq.h $(DESTDIR)$(INCLUDEDIR)/fbmq.h
	install -d $(DESTDIR)$(PKGCONFIGDIR)
	install -m 644 fbmq.pc $(DESTDIR)$(PKGCONFIGDIR)/fbmq.pc

# ── Uninstall ──

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)
	rm -f $(DESTDIR)$(BINDIR)/fbmq-reaper
	rm -f $(DESTDIR)$(BINDIR)/fbmq-worker
	rm -f $(DESTDIR)$(MANDIR)/man1/fbmq-worker.1
	rm -f $(DESTDIR)$(MANDIR)/man1/fbmq.1
	rm -f $(DESTDIR)$(MANDIR)/man5/fbmq-message.5
	rm -f $(DESTDIR)$(MANDIR)/man7/fbmq-design.7
	rm -f $(DESTDIR)$(LIBDIR)/$(STATIC_LIB)
	rm -f $(DESTDIR)$(LIBDIR)/$(SHARED_LIB)
	rm -f $(DESTDIR)$(LIBDIR)/$(SHARED_SONAME)
	rm -f $(DESTDIR)$(LIBDIR)/$(SHARED_LINKER)
	rm -f $(DESTDIR)$(INCLUDEDIR)/fbmq.h
	rm -f $(DESTDIR)$(PKGCONFIGDIR)/fbmq.pc

# ── Test ──

test: $(BIN)
	@bash test/test_fbmq.sh
