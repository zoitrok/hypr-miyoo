#include "net/ws.h"
#include "util/log.h"
#include "tap.h"

#include <string.h>

int main(void)
{
    log_init(LOG_ERROR);

    /* --- the RFC 6455 worked example -------------------------------------
     * Verifying the accept key is what distinguishes a real WebSocket peer
     * from a proxy that echoed a 101 without understanding the protocol. This
     * exact pair appears in the RFC, and the live server returned the same
     * value for the same key during development. */
    {
        char accept[64];
        CHECK(ws_compute_accept("dGhlIHNhbXBsZSBub25jZQ==", accept, sizeof(accept)),
              "accept key computed");
        CHECK_STR(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    }

    /* --- header parsing --------------------------------------------------- */
    ws_frame_hdr_t h;

    /* Unmasked text frame, 3-byte payload: the shape servers send. */
    {
        const uint8_t f[] = { 0x81, 0x03, 'a', 'b', 'c' };
        CHECK_INT(ws_parse_header(f, sizeof(f), &h), 1);
        CHECK_INT(h.fin, 1);
        CHECK_INT(h.opcode, WS_OP_TEXT);
        CHECK_INT(h.masked, 0);
        CHECK_INT(h.payload_len, 3);
        CHECK_INT(h.header_len, 2);
    }

    /* A truncated header must ask for more, not guess. */
    {
        const uint8_t f[] = { 0x81 };
        CHECK_INT(ws_parse_header(f, 1, &h), 0);
        CHECK_INT(ws_parse_header(f, 0, &h), 0);
    }

    /* 16-bit extended length. */
    {
        const uint8_t f[] = { 0x81, 126, 0x01, 0x00 };
        CHECK_INT(ws_parse_header(f, sizeof(f), &h), 1);
        CHECK_INT(h.payload_len, 256);
        CHECK_INT(h.header_len, 4);
        CHECK_INT(ws_parse_header(f, 3, &h), 0); /* needs all 4 */
    }

    /* 64-bit extended length. The initial snapshot is tens of KB, so this path
     * is exercised in normal operation and not merely in theory. */
    {
        const uint8_t f[] = { 0x82, 127, 0, 0, 0, 0, 0, 0x05, 0x00, 0x00 };
        CHECK_INT(ws_parse_header(f, sizeof(f), &h), 1);
        CHECK_INT(h.payload_len, 0x050000);
        CHECK_INT(h.header_len, 10);
        CHECK_INT(h.opcode, WS_OP_BINARY);
    }

    /* Lengths must use the shortest encoding that fits; a peer padding them is
     * either broken or trying something. */
    {
        const uint8_t small_in_16[] = { 0x81, 126, 0x00, 0x05 };
        CHECK_INT(ws_parse_header(small_in_16, sizeof(small_in_16), &h), -1);

        const uint8_t small_in_64[] = { 0x81, 127, 0, 0, 0, 0, 0, 0, 0x01, 0x00 };
        CHECK_INT(ws_parse_header(small_in_64, sizeof(small_in_64), &h), -1);
    }

    /* The high bit of a 64-bit length must be clear per RFC 6455. */
    {
        const uint8_t huge[] = { 0x81, 127, 0x80, 0, 0, 0, 0, 0, 0, 0 };
        CHECK_INT(ws_parse_header(huge, sizeof(huge), &h), -1);
    }

    /* Reserved bits mean an extension we never negotiated. */
    {
        const uint8_t rsv[] = { 0xC1, 0x00 };
        CHECK_INT(ws_parse_header(rsv, sizeof(rsv), &h), -1);
    }

    /* Control frames: at most 125 bytes, never fragmented. */
    {
        const uint8_t long_ping[] = { 0x89, 126, 0x00, 0xff };
        CHECK_INT(ws_parse_header(long_ping, sizeof(long_ping), &h), -1);

        const uint8_t fragmented_ping[] = { 0x09, 0x02 };
        CHECK_INT(ws_parse_header(fragmented_ping, sizeof(fragmented_ping), &h), -1);
    }

    /* A non-final text frame is normal -- large snapshots arrive fragmented. */
    {
        const uint8_t frag[] = { 0x01, 0x02 };
        CHECK_INT(ws_parse_header(frag, sizeof(frag), &h), 1);
        CHECK_INT(h.fin, 0);
        CHECK_INT(h.opcode, WS_OP_TEXT);

        const uint8_t cont[] = { 0x80, 0x02 };
        CHECK_INT(ws_parse_header(cont, sizeof(cont), &h), 1);
        CHECK_INT(h.fin, 1);
        CHECK_INT(h.opcode, WS_OP_CONTINUATION);
    }

    /* --- header building -------------------------------------------------- */
    {
        static const uint8_t MASK[4] = { 0xde, 0xad, 0xbe, 0xef };
        uint8_t buf[WS_MAX_HEADER];

        /* Clients must always mask; the 0x80 bit on byte 1 says so. */
        size_t n = ws_build_header(buf, WS_OP_TEXT, true, 5, MASK);
        CHECK_INT(n, 6);
        CHECK_INT(buf[0], 0x81);
        CHECK_INT(buf[1], 0x85);
        CHECK(memcmp(buf + 2, MASK, 4) == 0, "mask key written");

        n = ws_build_header(buf, WS_OP_TEXT, true, 200, MASK);
        CHECK_INT(n, 8);
        CHECK_INT(buf[1], 0x80 | 126);

        n = ws_build_header(buf, WS_OP_BINARY, true, 70000, MASK);
        CHECK_INT(n, 14);
        CHECK_INT(buf[1], 0x80 | 127);

        /* Everything we build must parse back to what we meant. */
        for (uint64_t len = 0; len <= 70000; len = len ? len * 7 : 1) {
            uint8_t b[WS_MAX_HEADER];
            size_t hn = ws_build_header(b, WS_OP_TEXT, true, len, MASK);
            ws_frame_hdr_t got;
            int rc = ws_parse_header(b, hn, &got);
            CHECK(rc == 1 && got.payload_len == len && got.masked &&
                  got.header_len == hn,
                  "build/parse round-trip at length %llu",
                  (unsigned long long)len);
        }
    }

    /* --- masking ---------------------------------------------------------- */
    {
        static const uint8_t MASK[4] = { 0x01, 0x02, 0x03, 0x04 };
        char data[] = "hello websocket";
        char orig[sizeof(data)];
        memcpy(orig, data, sizeof(data));

        ws_apply_mask((uint8_t *)data, strlen(data), MASK, 0);
        CHECK(memcmp(data, orig, strlen(orig)) != 0, "masking changed the data");

        ws_apply_mask((uint8_t *)data, strlen(data), MASK, 0);
        CHECK_STR(data, orig);

        /* Masking must be applicable in pieces, because payloads are sent in
         * chunks and the key position depends on the offset within the whole
         * payload, not within the chunk. */
        char piecewise[] = "hello websocket";
        size_t len = strlen(piecewise);
        ws_apply_mask((uint8_t *)piecewise, len, MASK, 0);

        char rebuilt[sizeof(piecewise)];
        memcpy(rebuilt, piecewise, sizeof(piecewise));
        size_t off = 0;
        while (off < len) {
            size_t n = (len - off > 3) ? 3 : len - off;
            ws_apply_mask((uint8_t *)rebuilt + off, n, MASK, off);
            off += n;
        }
        CHECK_STR(rebuilt, orig);
    }

    TAP_DONE();
}
