#!/bin/sh
# OnionOS entry point for Hypr Radio.

mydir=$(dirname "$0")
cd "$mydir" || exit 1

# HOME is not usefully set when MainUI launches an App, and anything that falls
# back to it would write into the SD card root.
export HOME="$mydir"

# Our own lib/ first (currently empty -- mbedTLS is linked statically), then the
# OnionOS system libraries. libSDL-1.2 comes from /mnt/SDCARD/miyoo/lib on
# purpose: that build is patched for the SigmaStar MI_GFX/MI_AO blocks and the
# physically rotated panel, and a bundled copy would lose all of it.
export LD_LIBRARY_PATH="$mydir/lib:/mnt/SDCARD/miyoo/lib:/mnt/SDCARD/.tmp_update/lib:$LD_LIBRARY_PATH"

# Deliberately NOT setting SDL_VIDEODRIVER. "mmiyoo" is a steward-fu SDL2
# convention; against the SDL 1.2 that Onion ships it simply fails to find a
# driver. The patched SDL picks the right one by itself.

# Named hypr-log.txt, not log.txt: the probe app writes its own log in a
# sibling directory and two files with the same name are easy to mix up.
#
# Truncate rather than append: an append-only log on a device that runs for
# hours would grow without bound on a card we do not own.
exec ./hypr --ca ./ca.crt > "$mydir/hypr-log.txt" 2>&1
