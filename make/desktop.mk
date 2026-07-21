# Host build -- the primary development target.
#
# On Arch, `sdl` resolves to sdl12-compat, which maps the SDL 1.2 API onto SDL2.
# That is a feature rather than a compromise: it is actively maintained, and it
# exercises exactly the same API surface the device's patched SDL 1.2 exposes,
# so code that works here works there.

CC     ?= cc
STRIP  ?= strip

CFLAGS  += -O1 -g -DTARGET_DESKTOP
CFLAGS  += -fno-omit-frame-pointer

# Sanitizers on the host only. They catch exactly the class of bug (buffer
# overruns in frame parsers, races between the decode and render threads) that
# is nearly impossible to diagnose from a log file on a handheld.
ifeq ($(SANITIZE),1)
CFLAGS  += -fsanitize=address,undefined
LDFLAGS += -fsanitize=address,undefined
endif

SDL_CFLAGS := $(shell pkg-config --cflags sdl 2>/dev/null)
SDL_LIBS   := $(shell pkg-config --libs sdl 2>/dev/null)

ifeq ($(SDL_LIBS),)
$(warning SDL 1.2 not found via pkg-config -- install sdl12-compat to build the UI)
endif

CFLAGS    += $(SDL_CFLAGS)
SDL_LDLIBS := $(SDL_LIBS)

# mbedTLS is built with the host defaults; only the shared config defines matter.
MBEDTLS_CFLAGS := -O2
