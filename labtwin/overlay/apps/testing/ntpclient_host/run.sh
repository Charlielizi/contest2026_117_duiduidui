#!/usr/bin/env bash
set -euo pipefail
test_root=$(cd "$(dirname "$0")" && pwd)
apps_root=$(cd "$test_root/../.." && pwd)
test_output=$(mktemp -d)
trap 'rm -f "$test_output/ntpclient-test" "$test_output/ntpcstart-test"; rmdir "$test_output"' EXIT
for source in ntpclient_host_test ntpcstart_host_test; do
  gcc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-unused-function -Wno-sign-compare -Wno-missing-braces \
    -ffunction-sections -fdata-sections -Wl,--gc-sections -pthread \
    -I"$test_root/include" -I"$apps_root/include" \
    "$test_root/$source.c" -o "$test_output/${source%%_host_test}-test"
done
"$test_output/ntpclient-test"
"$test_output/ntpcstart-test"
