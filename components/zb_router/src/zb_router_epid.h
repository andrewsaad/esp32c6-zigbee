/*
 * Extended PAN ID option parsing (see CONFIG_ZB_ROUTER_EXT_PAN_ID).
 * Private to the zb_router component.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Parse 16 hex chars, big-endian as ZHA displays it, colons ignored, into
 * the little-endian esp_zb_ieee_addr_t layout. False on malformed input. */
bool zb_router_epid_parse(const char *str, uint8_t out[8]);

/* True when all 8 bytes are zero ("join any network"). */
bool zb_router_epid_is_zero(const uint8_t epid[8]);
