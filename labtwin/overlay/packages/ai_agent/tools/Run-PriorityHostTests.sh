#!/usr/bin/env bash
set -euo pipefail
OPENVELA_ROOT=${OPENVELA_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}
cd "${AI_AGENT_TEST_ROOT:-${OPENVELA_ROOT}/packages/ai_agent}"
CJSON=${OPENVELA_ROOT}/apps/netutils/cjson/cJSON
test_root=$(mktemp -d /tmp/labtwin-priority-XXXXXX)
gcc -std=gnu11 -Wall -Wextra -Werror -DLABTWIN_HOST_TEST -DLABTWIN_ENV_HOST_TEST \
  "-DLABTWIN_ROOT=\"$test_root\"" -Iinclude -Isrc -I"$CJSON" \
  tests/labtwin_transaction_host_test.c src/labtwin/labtwin_environment.c \
  "$CJSON/cJSON.c" -lpthread -lm -o "$test_root/transaction-test"
"$test_root/transaction-test"
source tools/Prepare-HostTls.sh
gcc -std=gnu11 -Wall -Wextra -Werror -DAGENT_HOST_TEST -DOK=0 -DERROR=-1 \
  -DLABTWIN_HOST_TEST -DLABTWIN_ENV_HOST_TEST "-DLABTWIN_ROOT=\"$test_root\"" \
  "-DPORTAL_LEDGER_PATH=\"$test_root/portal-ledger.json\"" \
  -Iinclude -Isrc -I"$CJSON" -I"$MBEDTLS/include" \
  tests/portal_operations_host_test.c src/labtwin/labtwin_service.c src/labtwin/labtwin_environment.c \
  "$CJSON/cJSON.c" "$HOST_LIB/libmbedcrypto.a" -lpthread -lm -o "$test_root/operations-test"
"$test_root/operations-test"
gcc -std=gnu11 -Wall -Wextra -Werror -Isrc tests/http_byte_range_host_test.c -o "$test_root/range-test"
"$test_root/range-test"
