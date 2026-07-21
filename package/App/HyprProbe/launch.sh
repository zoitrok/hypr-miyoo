#!/bin/sh
# Phase 0 hardware probe. Run once, then copy log.txt back off the card.
#
# Safe to run: no network, writes only inside its own directory, and it exits
# on its own after two minutes even if no button does what we expect.

mydir=$(dirname "$0")
cd "$mydir" || exit 1
export HOME="$mydir"
export LD_LIBRARY_PATH="$mydir/lib:/mnt/SDCARD/miyoo/lib:/mnt/SDCARD/.tmp_update/lib:$LD_LIBRARY_PATH"

exec ./probe > "$mydir/probe-log.txt" 2>&1
