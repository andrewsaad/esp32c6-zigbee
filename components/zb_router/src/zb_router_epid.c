/*
 * Extended PAN ID option parsing. Pure C with no ESP-IDF dependencies, so it
 * is unit-tested on the host (test/host/).
 *
 * SPDX-License-Identifier: MIT
 */
#include "zb_router_epid.h"

#include <ctype.h>

/* Parse the configured EPID: 16 hex chars, big-endian as ZHA displays it,
 * colons ignored. esp_zb_ieee_addr_t (uint8_t[8]) is little-endian (LSB first), so the
 * first hex pair lands in out[7]. Returns false on any malformed input. */
bool zb_router_epid_parse(const char *str, uint8_t out[8])
{
    int nibbles = 0;
    for (const char *c = str; *c != '\0'; c++) {
        if (*c == ':') {
            continue;
        }
        if (!isxdigit((unsigned char)*c) || nibbles >= 16) {
            return false;
        }
        uint8_t v = (uint8_t)(isdigit((unsigned char)*c) ? *c - '0'
                                                         : tolower((unsigned char)*c) - 'a' + 10);
        int byte = 7 - (nibbles / 2);
        out[byte] = (nibbles % 2 == 0) ? (uint8_t)(v << 4) : (uint8_t)(out[byte] | v);
        nibbles++;
    }
    return nibbles == 16;
}

bool zb_router_epid_is_zero(const uint8_t epid[8])
{
    for (int i = 0; i < 8; i++) {
        if (epid[i] != 0) {
            return false;
        }
    }
    return true;
}
