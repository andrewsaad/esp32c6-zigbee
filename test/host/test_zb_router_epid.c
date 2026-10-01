/*
 * Host unit test for components/zb_router/src/zb_router_epid.c.
 * Build and run from the repo root (or use test/host/run.sh):
 *   gcc -Wall -Wextra -Werror -I components/zb_router/src \
 *       -o test/host/test_zb_router_epid \
 *       test/host/test_zb_router_epid.c components/zb_router/src/zb_router_epid.c \
 *       && ./test/host/test_zb_router_epid
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "zb_router_epid.h"

static int failures;

static void check(const char *str, bool ok, const uint8_t *expected)
{
    uint8_t out[8] = { 0 };
    bool r = zb_router_epid_parse(str, out);
    bool pass = (r == ok) && (!ok || memcmp(out, expected, 8) == 0);
    printf("%s \"%s\"\n", pass ? "PASS" : "FAIL", str);
    failures += !pass;
}

int main(void)
{
    /* ZHA shows 01:23:45:67:89:ab:cd:ef; the SDK wants it LSB first. */
    const uint8_t le[8] = { 0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01 };
    const uint8_t zero[8] = { 0 };

    check("01:23:45:67:89:ab:cd:ef", true, le);
    check("0123456789ABCDEF", true, le);
    check("0000000000000000", true, zero);
    check("0123456789abcde", false, NULL);    /* too short */
    check("0123456789abcdef00", false, NULL); /* too long */
    check("zz23456789abcdef", false, NULL);   /* not hex */
    check("", false, NULL);

    uint8_t epid[8];
    zb_router_epid_parse("0000000000000000", epid);
    bool zero_ok = zb_router_epid_is_zero(epid);
    zb_router_epid_parse("0000000000000001", epid);
    zero_ok = zero_ok && !zb_router_epid_is_zero(epid);
    printf("%s zb_router_epid_is_zero\n", zero_ok ? "PASS" : "FAIL");
    failures += !zero_ok;

    printf("%d failure(s)\n", failures);
    return failures != 0;
}
