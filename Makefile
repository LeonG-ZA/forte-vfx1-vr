# DOSBox VFX1 bridge -- plain make build.
#
# No cmake, no autotools, no external libraries beyond libm, because the whole
# point of libvfxbridge is that it drags nothing in with it. CMakeLists.txt is
# provided too for anyone who prefers it.

CC      ?= cc
CSTD    ?= -std=c99
WARN     = -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes \
           -Wmissing-prototypes -Wconversion -Wno-sign-conversion
OPT     ?= -O2
CFLAGS  += $(CSTD) $(WARN) $(OPT) -Iinclude -Isrc
LDLIBS  += -lm

BUILD   := build
LIB     := $(BUILD)/libvfxbridge.a

BRIDGE_SRC := src/bridge/vfxbridge.c src/bridge/stereo.c \
              src/bridge/reproject.c src/bridge/vfxmath.c \
              src/bridge/mousemap.c
BRIDGE_OBJ := $(BRIDGE_SRC:%.c=$(BUILD)/%.o)

WSPROTO_SRC := src/wsproto/wsproto.c
WSPROTO_OBJ := $(WSPROTO_SRC:%.c=$(BUILD)/%.o)
WSPROTO_LIB := $(BUILD)/libwsproto.a

VFX1DEV_SRC := src/vfx1dev/vfx1dev.c src/vrtrack/vrtrack.c
VFX1DEV_OBJ := $(VFX1DEV_SRC:%.c=$(BUILD)/%.o)
VFX1DEV_LIB := $(BUILD)/libvfx1dev.a

VFXHOST_SRC := src/vfxhost/vfxhost.c src/vfxmenu/vfxmenu.c src/vfxmenu/vgafont16.c src/vfxdepth/vfxdepth.c
VFXHOST_OBJ := $(VFXHOST_SRC:%.c=$(BUILD)/%.o)
VFXHOST_LIB := $(BUILD)/libvfxhost.a

CORE_SRC := src/core_stub/stub_core.c
CORE_OBJ := $(CORE_SRC:%.c=$(BUILD)/%.o)

HEADLESS_SRC := src/adapters/headless/main.c src/adapters/headless/png_write.c
HEADLESS_OBJ := $(HEADLESS_SRC:%.c=$(BUILD)/%.o)
HEADLESS_BIN := $(BUILD)/vfx-headless

# libretro frontend shared with the native app, and its headless test bench
RETROHOST_SRC := src/retrohost/retrohost.c
RETROHOST_OBJ := $(RETROHOST_SRC:%.c=$(BUILD)/%.o)
RETRO_HEADLESS_OBJ := $(BUILD)/src/adapters/libretro-headless/main.o $(BUILD)/src/adapters/headless/png_write.o
RETRO_HEADLESS_BIN := $(BUILD)/vfx-retro-headless

TEST_SRC := $(wildcard tests/test_*.c)
TEST_BIN := $(TEST_SRC:tests/%.c=$(BUILD)/%)

.PHONY: all clean test check layering run
all: $(LIB) $(WSPROTO_LIB) $(VFX1DEV_LIB) $(VFXHOST_LIB) $(HEADLESS_BIN) $(RETRO_HEADLESS_BIN)

$(LIB): $(BRIDGE_OBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(WSPROTO_LIB): $(WSPROTO_OBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(VFX1DEV_LIB): $(VFX1DEV_OBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(VFXHOST_LIB): $(VFXHOST_OBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(BUILD)/src/retrohost/%.o: CFLAGS += -Ithird_party/libretro -Wno-pedantic -Wno-strict-prototypes
$(RETRO_HEADLESS_BIN): $(RETRO_HEADLESS_OBJ) $(RETROHOST_OBJ) $(VFXHOST_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS) -ldl

$(HEADLESS_BIN): $(HEADLESS_OBJ) $(CORE_OBJ) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Tests link the stub core as well, since most of them drive a full loop.
$(BUILD)/test_%: tests/test_%.c $(CORE_OBJ) $(LIB) $(WSPROTO_LIB) $(VFX1DEV_LIB) $(VFXHOST_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Itests -o $@ $< $(CORE_OBJ) $(LIB) $(WSPROTO_LIB) $(VFX1DEV_LIB) $(VFXHOST_LIB) $(LDLIBS)

test: $(TEST_BIN)
	@fail=0; \
	for t in $(TEST_BIN); do \
	  printf '%-28s ' "$$(basename $$t)"; \
	  if ./$$t > $(BUILD)/$$(basename $$t).log 2>&1; then \
	    echo PASS; \
	  else \
	    echo FAIL; sed 's/^/    /' $(BUILD)/$$(basename $$t).log; fail=1; \
	  fi; \
	done; \
	exit $$fail

layering:
	@sh tools/check_layering.sh

# The full gate: the layering rule is a hard requirement, so it is
# a test, not a code-review convention.
check: layering test

run: $(HEADLESS_BIN)
	./$(HEADLESS_BIN) --out $(BUILD)/frames --seconds 2

# Only what this Makefile produces. build/ also holds the web site, packed game
# files, TLS certificates and benchmark logs from web/, and an earlier
# `rm -rf build` took all of those with it.
clean:
	rm -rf $(BUILD)/src $(BUILD)/tests $(BUILD)/frames
	rm -f $(BUILD)/*.a $(BUILD)/test_* $(BUILD)/vfx-headless $(BUILD)/vfx-retro-headless

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
