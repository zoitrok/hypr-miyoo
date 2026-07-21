# Miyoo Mini Plus build.
#
# Run inside the union toolchain container (`make shell` in
# shauninman/union-miyoomini-toolchain), which exports CROSS_COMPILE and PATH.
#
# Linking policy, which is deliberate and worth not "fixing":
#
#   SDL 1.2  -- linked DYNAMICALLY against the copy OnionOS already ships at
#               /mnt/SDCARD/miyoo/lib. That build is patched to drive the
#               SigmaStar MI_GFX/MI_AO blocks and to handle the physically
#               rotated panel. Bundling our own would lose all of that. The
#               1.2.15 ABI is frozen, so this is stable across Onion releases.
#
#   mbedTLS  -- linked STATICALLY from our own build. The device does ship
#               libmbedtls.so.10, but it is mbedTLS 2.x and its version drifts
#               between Onion releases. ~300KB buys immunity to that.

# Use the toolchain installed by tools/fetch-toolchain.sh unless the caller
# supplies its own (e.g. running inside the union toolchain container, which
# exports CROSS_COMPILE itself).
MIYOO_TOOLCHAIN_DIR ?= $(if $(XDG_DATA_HOME),$(XDG_DATA_HOME),$(HOME)/.local/share)/miyoomini-toolchain
TOOLCHAIN_CC := $(MIYOO_TOOLCHAIN_DIR)/miyoomini-toolchain/usr/bin/arm-linux-gnueabihf-gcc

ifndef CROSS_COMPILE
ifneq ($(wildcard $(TOOLCHAIN_CC)),)
CROSS_COMPILE := $(MIYOO_TOOLCHAIN_DIR)/miyoomini-toolchain/usr/bin/arm-linux-gnueabihf-
else
$(error No ARM toolchain found. Run ./tools/fetch-toolchain.sh, or set CROSS_COMPILE)
endif
endif

CC     := $(CROSS_COMPILE)gcc
AR     := $(CROSS_COMPILE)ar
STRIP  := $(CROSS_COMPILE)strip

SYSROOT ?= $(shell $(CC) -print-sysroot)

# -ffast-math is safe for us: the only float work is the FFT and the perspective
# grid, neither of which cares about NaN semantics or strict IEEE rounding.
CFLAGS  += -O2 -DTARGET_MIYOO \
           -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard \
           -ffast-math -fomit-frame-pointer \
           -I$(SYSROOT)/usr/include/SDL

LDFLAGS += -L$(SYSROOT)/usr/lib
SDL_LDLIBS := -lSDL

MBEDTLS_CFLAGS := -O2 -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard

MBEDTLS_CMAKE_FLAGS := \
	-DCMAKE_SYSTEM_NAME=Linux \
	-DCMAKE_SYSTEM_PROCESSOR=arm \
	-DCMAKE_C_COMPILER=$(CC)
