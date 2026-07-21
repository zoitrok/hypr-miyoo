#!/bin/sh
# Install the Miyoo Mini cross-toolchain.
#
# The upstream project (shauninman/union-miyoomini-toolchain) wraps this in a
# Docker image, but the image only ever downloads and extracts a self-contained
# tarball: GCC 8.3 for arm-linux-gnueabihf plus a buildroot sysroot with SDL 1.2.
# Those are ordinary x86_64 binaries, so we skip Docker and extract them
# directly -- fewer moving parts, and it works where Docker networking does not.
#
# GCC 8.3 matters: it targets an old enough glibc (we require at most
# GLIBC_2.17) that binaries run on the device's buildroot userland. A modern
# host cross-compiler would produce binaries the device cannot load.
#
# Installs outside the repo by default, because it unpacks to about a gigabyte
# and this project may live in a synced directory.
set -eu

TOOLCHAIN_VERSION=v0.0.3
TOOLCHAIN_TAR=miyoomini-toolchain.tar.xz
TOOLCHAIN_URL="https://github.com/shauninman/miyoomini-toolchain-buildroot/releases/download/${TOOLCHAIN_VERSION}/${TOOLCHAIN_TAR}"

: "${MIYOO_TOOLCHAIN_DIR:=${XDG_DATA_HOME:-$HOME/.local/share}/miyoomini-toolchain}"

GCC="$MIYOO_TOOLCHAIN_DIR/miyoomini-toolchain/usr/bin/arm-linux-gnueabihf-gcc"

if [ -x "$GCC" ]; then
    echo "toolchain: already installed at $MIYOO_TOOLCHAIN_DIR"
    "$GCC" --version | head -1
    exit 0
fi

cache="${XDG_CACHE_HOME:-$HOME/.cache}/miyoohypr"
mkdir -p "$cache" "$MIYOO_TOOLCHAIN_DIR"
tarball="$cache/$TOOLCHAIN_TAR"

if [ ! -f "$tarball" ]; then
    echo "toolchain: downloading ${TOOLCHAIN_VERSION} (267 MB)..."
    curl -sSfL -o "$tarball.part" "$TOOLCHAIN_URL"
    mv "$tarball.part" "$tarball"
fi

echo "toolchain: extracting to $MIYOO_TOOLCHAIN_DIR ..."
tar xf "$tarball" -C "$MIYOO_TOOLCHAIN_DIR"

if [ ! -x "$GCC" ]; then
    echo "toolchain: extraction did not produce $GCC" >&2
    exit 1
fi

echo "toolchain: ready"
"$GCC" --version | head -1
echo
echo "Now: make TARGET=device      (or: make package / make package-probe)"
