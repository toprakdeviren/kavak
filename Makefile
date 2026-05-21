# kavak — generic frontend kernel.
#
# Targets:
#   make / make debug   build static lib (debug, -O0 -g)
#   make release        optimized build (-O2)
#   make test           run unit tests
#   make sanitize       run unit tests under ASan/UBSan
#   make wasm           build a wasm-ready static libkavak.a
#   make examples       build runnable examples
#   make clean          remove build artifacts

CC      ?= clang
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic
ROOT    := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
PREFIX  ?= /usr/local
DESTDIR ?=

INCDIR    = $(ROOT)include
SRCDIR    = $(ROOT)src
TESTDIR   = $(ROOT)tests
EXAMPLEDIR = $(ROOT)examples
BUILDDIR  = $(ROOT)build
DECODER_ROOT ?= $(abspath $(ROOT)../../decoder)
DECODER_LIB  ?= $(DECODER_ROOT)/build/libunicode.a
DECODER_WASM_LIB ?= $(DECODER_ROOT)/build/wasm/libunicode.a
DECODER_INCLUDES = -I$(DECODER_ROOT)/include
DECODER_LDLIBS ?= -lm -lpthread
LIBTOOL ?= libtool
PKG_CONFIG_DIR ?= $(PREFIX)/lib/pkgconfig

INCLUDES  = -I$(INCDIR) $(DECODER_INCLUDES)

SRCS = \
  $(SRCDIR)/arena.c \
  $(SRCDIR)/source.c \
  $(SRCDIR)/span.c \
  $(SRCDIR)/diag.c \
  $(SRCDIR)/utf8.c \
  $(SRCDIR)/token.c \
  $(SRCDIR)/lexer.c \
  $(SRCDIR)/parser.c \
  $(SRCDIR)/ast.c \
  $(SRCDIR)/type.c \
  $(SRCDIR)/sema.c \
  $(SRCDIR)/dump.c \
  $(SRCDIR)/kavak.c

OBJS = $(patsubst $(ROOT)%.c, $(BUILDDIR)/%.o, $(SRCS))
DEPS = $(OBJS:.o=.d)
LIB  = $(BUILDDIR)/libkavak.a
PC   = $(BUILDDIR)/kavak.pc

.PHONY: all debug release test clean decoder-wasm wasm examples sanitize \
        pkgconfig install FORCE

all: debug

debug:   CFLAGS += -g -O0
debug:   $(LIB)

release: CFLAGS += -O2 -DNDEBUG
release: $(LIB)

define require_decoder_root
	@test -d "$(DECODER_ROOT)" || { \
	  echo "error: DECODER_ROOT not found: $(DECODER_ROOT)" >&2; \
	  echo "       set DECODER_ROOT=/absolute/path/to/decoder" >&2; \
	  exit 1; \
	}
endef

define merge_native_archive
	@rm -f $(1)
	@if command -v "$(LIBTOOL)" >/dev/null 2>&1 && \
	    "$(LIBTOOL)" -static -o $(1) $(2) $(3) >/dev/null 2>&1; then \
	  :; \
	else \
	  rm -f $(1); \
	  { \
	    printf "CRE%s %s\n" "ATE" "$(1)"; \
	    for obj in $(2); do echo "ADDMOD $$obj"; done; \
	    echo "ADDLIB $(3)"; \
	    echo "SAVE"; \
	    echo "END"; \
	  } | $(AR) -M; \
	fi
endef

$(DECODER_LIB): FORCE
	$(require_decoder_root)
	@$(MAKE) -C "$(DECODER_ROOT)" lib

$(LIB): $(OBJS) $(DECODER_LIB) Makefile
	@mkdir -p $(dir $@)
	@printf "  %-7s %s\n" "AR" "$(notdir $@)"
	$(call merge_native_archive,$@,$(OBJS),$(DECODER_LIB))
	@echo "  ✓ libkavak"

$(BUILDDIR)/%.o: $(ROOT)%.c
	@mkdir -p $(dir $@)
	@printf "  %-7s %s\n" "CC" "$(notdir $<)"
	@$(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

-include $(DEPS)

# ── Tests ──────────────────────────────────────────────────────────────────
TEST_SRCS = \
  $(TESTDIR)/test_arena.c \
  $(TESTDIR)/test_source.c \
  $(TESTDIR)/test_span.c \
  $(TESTDIR)/test_diag.c \
  $(TESTDIR)/test_utf8.c \
  $(TESTDIR)/test_token.c \
  $(TESTDIR)/test_ast.c \
  $(TESTDIR)/test_type.c \
  $(TESTDIR)/test_sema.c \
  $(TESTDIR)/test_lexer.c \
  $(TESTDIR)/test_lexer_offside.c \
  $(TESTDIR)/test_parser.c

TEST_BINS = $(patsubst $(TESTDIR)/%.c, $(BUILDDIR)/tests/%, $(TEST_SRCS))

test: debug $(TEST_BINS)
	@for t in $(TEST_BINS); do \
	  printf "  %-7s %s\n" "RUN" "$$(basename $$t)"; \
	  $$t || exit $$?; \
	done

$(BUILDDIR)/tests/%: $(TESTDIR)/%.c $(LIB)
	@mkdir -p $(dir $@)
	@printf "  %-7s %s\n" "LINK" "$(notdir $@)"
	@$(CC) $(CFLAGS) $(INCLUDES) $< $(LIB) $(DECODER_LDLIBS) -o $@

# ── Examples ────────────────────────────────────────────────────────────────
EXAMPLE_SRCS = \
  $(EXAMPLEDIR)/tinylang.c
EXAMPLE_BINS = $(patsubst $(EXAMPLEDIR)/%.c, $(BUILDDIR)/examples/%, $(EXAMPLE_SRCS))

examples: debug $(EXAMPLE_BINS)

$(BUILDDIR)/examples/%: $(EXAMPLEDIR)/%.c $(LIB)
	@mkdir -p $(dir $@)
	@printf "  %-7s %s\n" "LINK" "$(notdir $@)"
	@$(CC) $(CFLAGS) $(INCLUDES) $< $(LIB) $(DECODER_LDLIBS) -o $@

# ── Benchmarks ──────────────────────────────────────────────────────────────
BENCH_SRCS = \
  $(TESTDIR)/bench_arena.c
BENCH_BUILDDIR = $(BUILDDIR)/bench
BENCH_OBJS = $(patsubst $(ROOT)%.c, $(BENCH_BUILDDIR)/%.o, $(SRCS))
BENCH_DEPS = $(BENCH_OBJS:.o=.d)
BENCH_LIB  = $(BENCH_BUILDDIR)/libkavak.a
BENCH_BINS = $(patsubst $(TESTDIR)/%.c, $(BENCH_BUILDDIR)/tests/%, $(BENCH_SRCS))
BENCH_CFLAGS = $(CFLAGS) -O2 -DNDEBUG

bench: $(BENCH_BINS)
	@for b in $(BENCH_BINS); do \
	  printf "  %-7s %s\n" "BENCH" "$$(basename $$b)"; \
	  echo ""; \
	  $$b; \
	  echo ""; \
	done

.PHONY: bench

$(BENCH_LIB): $(BENCH_OBJS) $(DECODER_LIB) Makefile
	@mkdir -p $(dir $@)
	@printf "  %-7s %s\n" "AR" "$(notdir $@)"
	$(call merge_native_archive,$@,$(BENCH_OBJS),$(DECODER_LIB))
	@echo "  ✓ bench libkavak"

$(BENCH_BUILDDIR)/%.o: $(ROOT)%.c
	@mkdir -p $(dir $@)
	@printf "  %-7s %s\n" "CC" "$(notdir $<)"
	@$(CC) $(BENCH_CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

-include $(BENCH_DEPS)

$(BENCH_BUILDDIR)/tests/%: $(TESTDIR)/%.c $(BENCH_LIB)
	@mkdir -p $(dir $@)
	@printf "  %-7s %s\n" "LINK" "$(notdir $@)"
	@$(CC) $(BENCH_CFLAGS) $(INCLUDES) $< $(BENCH_LIB) $(DECODER_LDLIBS) -o $@

# ── ThreadSanitizer smoke ───────────────────────────────────────────────────
# Re-builds sources + tests/test_threads.c with -fsanitize=thread and checks
# that independent sessions do not share mutable state.
TSAN_DIR    = $(BUILDDIR)/tsan
TSAN_FLAGS  = -std=c11 -Wall -Wextra -Wpedantic -fsanitize=thread -g -O1

tsan: $(TSAN_DIR)/test_threads
	@printf "  %-7s %s\n" "TSAN" "test_threads"
	@$<

$(TSAN_DIR)/test_threads: $(SRCS) $(TESTDIR)/test_threads.c $(DECODER_LIB)
	@mkdir -p $(TSAN_DIR)
	@printf "  %-7s %s\n" "LINK" "test_threads (TSan)"
	@$(CC) $(TSAN_FLAGS) $(INCLUDES) $(SRCS) $(TESTDIR)/test_threads.c \
	  $(DECODER_LIB) $(DECODER_LDLIBS) -o $@

.PHONY: tsan

# ── Address/undefined sanitizer sweep ──────────────────────────────────────
SAN_DIR   = $(BUILDDIR)/sanitize
SAN_FLAGS = -std=c11 -Wall -Wextra -Wpedantic -fsanitize=address,undefined -g -O1
SAN_BINS  = $(patsubst $(TESTDIR)/%.c, $(SAN_DIR)/tests/%, $(TEST_SRCS))

sanitize: $(SAN_BINS)
	@for t in $(SAN_BINS); do \
	  printf "  %-7s %s\n" "SAN" "$$(basename $$t)"; \
	  $$t || exit $$?; \
	done

$(SAN_DIR)/tests/%: $(TESTDIR)/%.c $(SRCS) $(DECODER_LIB)
	@mkdir -p $(dir $@)
	@printf "  %-7s %s\n" "LINK" "$(notdir $@) (ASan/UBSan)"
	@$(CC) $(SAN_FLAGS) $(INCLUDES) $(SRCS) $< \
	  $(DECODER_LIB) $(DECODER_LDLIBS) -o $@

# ── WebAssembly static library ─────────────────────────────────────────────
WASM_BUILDDIR = $(BUILDDIR)/wasm
WASM_CC ?= emcc
WASM_AR ?= emar
WASM_CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O3 -DNDEBUG
WASM_OBJS = $(patsubst $(ROOT)%.c, $(WASM_BUILDDIR)/%.o, $(SRCS))
WASM_DEPS = $(WASM_OBJS:.o=.d)
WASM_LIB  = $(WASM_BUILDDIR)/libkavak.a

decoder-wasm: $(DECODER_WASM_LIB)

$(DECODER_WASM_LIB): FORCE
	$(require_decoder_root)
	@$(MAKE) -C "$(DECODER_ROOT)" build/wasm/libunicode.a

wasm: $(WASM_LIB) $(DECODER_WASM_LIB)
	@echo "  decoder static: $(DECODER_WASM_LIB)"

$(WASM_LIB): $(WASM_OBJS) $(DECODER_WASM_LIB) Makefile
	@mkdir -p $(dir $@)
	@printf "  %-7s %s\n" "EMAR" "$(notdir $@)"
	@rm -f $@
	@{ \
	  printf "CRE%s %s\n" "ATE" "$@"; \
	  for obj in $(WASM_OBJS); do echo "ADDMOD $$obj"; done; \
	  echo "ADDLIB $(DECODER_WASM_LIB)"; \
	  echo "SAVE"; \
	  echo "END"; \
	} | $(WASM_AR) -M
	@echo "  ✓ wasm libkavak + decoder"

$(WASM_BUILDDIR)/%.o: $(ROOT)%.c
	@mkdir -p $(dir $@)
	@printf "  %-7s %s\n" "EMCC" "$(notdir $<)"
	@$(WASM_CC) $(WASM_CFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

-include $(WASM_DEPS)

# ── pkg-config / install ───────────────────────────────────────────────────
pkgconfig: $(PC)

$(PC): Makefile
	@mkdir -p $(dir $@)
	@printf "prefix=%s\n" "$(PREFIX)" > $@
	@printf "exec_prefix=\$${prefix}\n" >> $@
	@printf "libdir=\$${exec_prefix}/lib\n" >> $@
	@printf "includedir=\$${prefix}/include\n\n" >> $@
	@printf "Name: kavak\n" >> $@
	@printf "Description: Generic C frontend kernel\n" >> $@
	@printf "Version: 0.1.0\n" >> $@
	@printf "Libs: -L\$${libdir} -lkavak\n" >> $@
	@printf "Cflags: -I\$${includedir}\n" >> $@

install: release pkgconfig
	@install -d "$(DESTDIR)$(PREFIX)/include" \
	            "$(DESTDIR)$(PREFIX)/lib" \
	            "$(DESTDIR)$(PKG_CONFIG_DIR)"
	@install -m 644 "$(INCDIR)/kavak.h" "$(DESTDIR)$(PREFIX)/include/kavak.h"
	@install -m 644 "$(LIB)" "$(DESTDIR)$(PREFIX)/lib/libkavak.a"
	@install -m 644 "$(PC)" "$(DESTDIR)$(PKG_CONFIG_DIR)/kavak.pc"

clean:
	rm -rf $(BUILDDIR)

# Landing page (kavak.run) lives at minimobile/web/kavak/ — see web/kavak/Makefile.
