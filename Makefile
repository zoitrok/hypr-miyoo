# Hypr Radio -- vaporwave internet radio client for the Miyoo Mini Plus.
#
#   make                 build for the host (desktop dev target)
#   make TARGET=device   build for the Miyoo Mini Plus
#   make deps            fetch + build third-party sources (run once)
#   make test            build and run the native unit tests
#   make package         assemble the OnionOS App directory into dist/
#   make clean           remove build output for the current TARGET
#
# The device build must run inside the union toolchain container
# (https://github.com/shauninman/union-miyoomini-toolchain -- `make shell`),
# which sets CROSS_COMPILE and PATH.

TARGET ?= desktop

BUILD    := build/$(TARGET)
BIN      := $(BUILD)/bin
OBJ      := $(BUILD)/obj

# --------------------------------------------------------------------- sources

# Everything that does not touch SDL. Keeping the decode and network pipeline
# SDL-free is what lets the dev tools stay headless: they exercise the whole
# stack without a display, an audio device, or a window server.
CORE_SRC := \
	$(wildcard src/util/*.c) \
	$(wildcard src/net/*.c) \
	$(wildcard src/state/*.c) \
	$(wildcard src/dsp/*.c) \
	$(filter-out src/audio/audio_out.c,$(wildcard src/audio/*.c)) \
	third_party/cJSON/cJSON.c

SDL_SRC  := $(wildcard src/ui/*.c) src/audio/audio_out.c
APP_SRC  := $(wildcard src/main.c)

# Most tools are headless. The probe is the exception -- its whole job is to
# interrogate SDL on the real device -- so it gets its own link rule.
SDL_TOOL_SRC := tools/probe.c
TOOL_SRC := $(filter-out $(SDL_TOOL_SRC),$(wildcard tools/*.c))
TEST_SRC := $(wildcard test/*.c)

CORE_OBJ := $(addprefix $(OBJ)/,$(CORE_SRC:.c=.o))
SDL_OBJ  := $(addprefix $(OBJ)/,$(SDL_SRC:.c=.o))
APP_OBJ  := $(addprefix $(OBJ)/,$(APP_SRC:.c=.o))

# ------------------------------------------------------------------ third-party

MBEDTLS_SRC_DIR := third_party/mbedtls
MBEDTLS_BUILD   := build/mbedtls/$(TARGET)
MBEDTLS_LIBDIR  := $(MBEDTLS_BUILD)/library

# Applied to BOTH the mbedTLS build and our own sources, from this one place.
# MBEDTLS_THREADING_C changes the layout of mbedTLS context structs, so a
# mismatch between the two sides is a silent ABI break rather than a build
# error. See third_party/hypr_mbedtls_config.h.
MBEDTLS_DEFS := -DMBEDTLS_USER_CONFIG_FILE='<hypr_mbedtls_config.h>' \
                -I$(CURDIR)/third_party
# Link order matters: tls depends on x509 depends on crypto.
MBEDTLS_LIBS    := $(MBEDTLS_LIBDIR)/libmbedtls.a \
                   $(MBEDTLS_LIBDIR)/libmbedx509.a \
                   $(MBEDTLS_LIBDIR)/libmbedcrypto.a

# --------------------------------------------------------------- common flags

WARNINGS := -Wall -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes \
            -Wmissing-prototypes -Wno-unused-parameter

CFLAGS  += -std=c11 -D_GNU_SOURCE $(WARNINGS) \
           -Isrc -Ithird_party -I$(MBEDTLS_SRC_DIR)/include \
           $(MBEDTLS_DEFS) \
           -ffunction-sections -fdata-sections
LDFLAGS += -Wl,--gc-sections
LDLIBS  += -lpthread -lm

include make/$(TARGET).mk

# -------------------------------------------------------------------- targets

BINARIES := $(patsubst tools/%.c,$(BIN)/%,$(TOOL_SRC)) \
            $(patsubst tools/%.c,$(BIN)/%,$(SDL_TOOL_SRC))
ifneq ($(APP_SRC),)
BINARIES += $(BIN)/hypr
endif

.PHONY: all
all: $(BINARIES)

$(BIN)/hypr: $(APP_OBJ) $(SDL_OBJ) $(CORE_OBJ) $(MBEDTLS_LIBS)
	@mkdir -p $(@D)
	$(CC) $(APP_OBJ) $(SDL_OBJ) $(CORE_OBJ) $(MBEDTLS_LIBS) \
	      $(LDFLAGS) $(SDL_LDLIBS) $(LDLIBS) -o $@

# The probe interrogates SDL and nothing else -- it has no network code, so it
# deliberately does not link the core or mbedTLS. That keeps it small, and means
# building it for the device needs only the cross-compiler and SDL headers.
PROBE_OBJ := $(OBJ)/src/util/mono.o

$(BIN)/probe: $(OBJ)/tools/probe.o $(PROBE_OBJ)
	@mkdir -p $(@D)
	$(CC) $^ $(LDFLAGS) $(SDL_LDLIBS) $(LDLIBS) -o $@

# Dev tools are headless on purpose: they exercise the network and audio stack
# without pulling in SDL, so they run anywhere and start instantly.
$(BIN)/%: $(OBJ)/tools/%.o $(CORE_OBJ) $(MBEDTLS_LIBS)
	@mkdir -p $(@D)
	$(CC) $< $(CORE_OBJ) $(MBEDTLS_LIBS) $(LDFLAGS) $(LDLIBS) -o $@

$(OBJ)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(shell find $(OBJ) -name '*.d' 2>/dev/null)

# ----------------------------------------------------------------------- deps

.PHONY: deps
deps: $(MBEDTLS_LIBS)

$(MBEDTLS_SRC_DIR)/include/mbedtls/version.h:
	./tools/fetch-deps.sh

$(MBEDTLS_LIBS): $(MBEDTLS_SRC_DIR)/include/mbedtls/version.h
	cmake -S $(MBEDTLS_SRC_DIR) -B $(MBEDTLS_BUILD) \
	      -DCMAKE_BUILD_TYPE=Release \
	      -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF \
	      -DUSE_STATIC_MBEDTLS_LIBRARY=ON -DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
	      -DCMAKE_C_FLAGS="$(MBEDTLS_CFLAGS) $(MBEDTLS_DEFS)" \
	      $(MBEDTLS_CMAKE_FLAGS)
	cmake --build $(MBEDTLS_BUILD) --parallel

# ----------------------------------------------------------------------- test

TEST_BINS := $(patsubst test/%.c,$(BIN)/test_%,$(TEST_SRC))

# Depends on `all` deliberately. Without it, `make test` leaves the app and
# tools binaries stale, and a passing test suite sitting next to a stale binary
# is worse than no test suite -- it invites you to trust a measurement of code
# you did not build.
.PHONY: test
test: all $(TEST_BINS)
	@for t in $(TEST_BINS); do \
		echo "--- $$t"; $$t || exit 1; \
	done
	@echo "all tests passed"

$(BIN)/test_%: $(OBJ)/test/%.o $(CORE_OBJ) $(MBEDTLS_LIBS)
	@mkdir -p $(@D)
	$(CC) $< $(CORE_OBJ) $(MBEDTLS_LIBS) $(LDFLAGS) $(LDLIBS) -o $@

# -------------------------------------------------------------------- package

# Packaging always re-enters make with TARGET=device and does the whole job
# there. Doing the copy and strip in the outer make would use the *host's*
# $(BIN) and $(STRIP) -- host strip cannot read an ARM binary, and the outer
# $(BIN) points at the desktop tree.

.PHONY: package package-probe
package:
	@$(MAKE) TARGET=device do-package
package-probe:
	@$(MAKE) TARGET=device do-package-probe

.PHONY: do-package do-package-probe
do-package: $(BIN)/hypr
	rm -rf dist/App/Hypr
	mkdir -p dist/App/Hypr/lib
	cp -r package/App/Hypr/. dist/App/Hypr/
	cp $(BIN)/hypr dist/App/Hypr/hypr
	$(STRIP) dist/App/Hypr/hypr
	chmod +x dist/App/Hypr/launch.sh dist/App/Hypr/hypr
	@echo "packaged -> dist/App/Hypr (copy to /mnt/SDCARD/App/)"

# The Phase 0 probe, packaged on its own so it can go to the device without the
# app. Run it once, copy log.txt back, then delete the directory.
do-package-probe: $(BIN)/probe
	rm -rf dist/App/HyprProbe
	mkdir -p dist/App/HyprProbe/lib
	cp -r package/App/HyprProbe/. dist/App/HyprProbe/
	cp $(BIN)/probe dist/App/HyprProbe/probe
	$(STRIP) dist/App/HyprProbe/probe
	chmod +x dist/App/HyprProbe/launch.sh dist/App/HyprProbe/probe
	@echo "packaged -> dist/App/HyprProbe (copy to /mnt/SDCARD/App/)"

# ---------------------------------------------------------------------- clean

.PHONY: clean
clean:
	rm -rf $(BUILD)

.PHONY: distclean
distclean:
	rm -rf build dist third_party/mbedtls third_party/.cache
