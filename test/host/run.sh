#!/bin/sh
# Build and run the host unit tests (needs gcc; on Windows run under WSL).
# SPDX-License-Identifier: MIT
set -e
cd "$(dirname "$0")/../.."
gcc -Wall -Wextra -Werror -I components/zb_router/src \
    -o test/host/test_zb_router_epid \
    test/host/test_zb_router_epid.c components/zb_router/src/zb_router_epid.c
./test/host/test_zb_router_epid
