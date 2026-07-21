#!/bin/sh
# Fetch third-party sources that are too large to vendor into the repo.
#
# minimp3 and cJSON are committed directly (a few files each). mbedTLS is not:
# it is a ~50MB source tree, so we fetch and checksum-verify a pinned release
# tarball instead. We build it ourselves for both the desktop and device
# targets rather than using the host's or buildroot's copy, so that both
# targets link the exact same version and API -- mbedTLS 2.x and 3.x differ
# enough (mbedtls_sha1_ret vs mbedtls_sha1, config header layout) that mixing
# them across targets is a real source of build breakage.
set -eu

MBEDTLS_VERSION=3.6.7
MBEDTLS_SHA256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
MBEDTLS_URL="https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-${MBEDTLS_VERSION}/mbedtls-${MBEDTLS_VERSION}.tar.bz2"

root=$(cd "$(dirname "$0")/.." && pwd)
vendor="$root/third_party"
cache="$vendor/.cache"
dest="$vendor/mbedtls"

if [ -f "$dest/include/mbedtls/version.h" ]; then
    echo "mbedtls: already present at third_party/mbedtls (rm -rf to refetch)"
    exit 0
fi

mkdir -p "$cache"
tarball="$cache/mbedtls-${MBEDTLS_VERSION}.tar.bz2"

if [ ! -f "$tarball" ]; then
    echo "mbedtls: downloading ${MBEDTLS_VERSION}..."
    curl -sSfL -o "$tarball.part" "$MBEDTLS_URL"
    mv "$tarball.part" "$tarball"
fi

echo "mbedtls: verifying checksum..."
actual=$(sha256sum "$tarball" | cut -d' ' -f1)
if [ "$actual" != "$MBEDTLS_SHA256" ]; then
    echo "mbedtls: CHECKSUM MISMATCH" >&2
    echo "  expected $MBEDTLS_SHA256" >&2
    echo "  actual   $actual" >&2
    echo "  refusing to unpack; delete $tarball and retry" >&2
    exit 1
fi

echo "mbedtls: unpacking..."
tmp="$vendor/.unpack.$$"
rm -rf "$tmp"
mkdir -p "$tmp"
tar -xjf "$tarball" -C "$tmp"
mv "$tmp/mbedtls-${MBEDTLS_VERSION}" "$dest"
rmdir "$tmp"

echo "mbedtls: ready at third_party/mbedtls"
