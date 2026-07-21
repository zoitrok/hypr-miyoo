/* mbedTLS user config for Hypr Radio.
 *
 * Included by mbedTLS via -DMBEDTLS_USER_CONFIG_FILE, on top of its defaults.
 *
 * CRITICAL: this must be applied identically to the mbedTLS build AND to every
 * translation unit of ours that includes an mbedTLS header. MBEDTLS_THREADING_C
 * adds a mutex member to context structs, so a mismatch is a silent ABI
 * mismatch -- different sizeof() on each side of the link, which corrupts
 * memory rather than failing to build. The Makefile drives both from a single
 * MBEDTLS_DEFS variable for exactly that reason. */

#ifndef HYPR_MBEDTLS_CONFIG_H
#define HYPR_MBEDTLS_CONFIG_H

/* The audio stream and the WebSocket are separate threads that share one
 * TLS context -- one entropy pool, one RNG, one parsed CA chain -- because
 * parsing and holding those twice is wasted RAM on a 128MB device.
 *
 * Sharing them is only safe with threading enabled. Without it, two threads
 * handshaking at once both draw from the CTR-DRBG concurrently and corrupt its
 * state; observed in practice as an intermittent
 * "tls handshake: ERROR - Generic error (-0x0001)" at startup, where the audio
 * and metadata connections race. It recovers via reconnect, which is exactly
 * what makes it the kind of bug that survives testing and shows up on device. */
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_PTHREAD

#endif
