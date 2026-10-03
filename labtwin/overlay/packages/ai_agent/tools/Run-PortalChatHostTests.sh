#!/usr/bin/env bash
set -euo pipefail
OPENVELA_ROOT=${OPENVELA_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}
cd "${AI_AGENT_TEST_ROOT:-${OPENVELA_ROOT}/packages/ai_agent}"
CJSON=${OPENVELA_ROOT}/apps/netutils/cjson/cJSON
source tools/Prepare-HostTls.sh
chat_test_root=$(mktemp -d /tmp/portal-chat-test-XXXXXX)
gcc -std=gnu11 -Wall -Wextra -Werror -DAGENT_HOST_TEST -DOK=0 -DERROR=-1 \
  "-DPORTAL_CHAT_PATH=\"$chat_test_root/ledger.json\"" \
  -Iinclude -Isrc -I"$CJSON" -I"$MBEDTLS/include" \
  tests/portal_chat_host_test.c "$CJSON/cJSON.c" "$HOST_LIB/libmbedcrypto.a" \
  -lpthread -lm -o "$chat_test_root/test"
"$chat_test_root/test"
gcc -std=gnu11 -Wall -Wextra -Werror -DAGENT_HOST_TEST -DOK=0 -DERROR=-1 \
  "-DCONFIG_EXAMPLES_AI_AGENT_VELA_DATA_DIR=\"$chat_test_root\"" \
  -Iinclude -Isrc -I"$CJSON" tests/portal_chat_files_host_test.c "$CJSON/cJSON.c" \
  -lm -o "$chat_test_root/files-test"
"$chat_test_root/files-test"
gcc -std=gnu11 -Wall -Wextra -Werror -Isrc tests/agent_start_gate_host_test.c \
  src/core/agent_start_gate.c -lpthread -o "$chat_test_root/start-test"
"$chat_test_root/start-test"
echo "Evidence retained at $chat_test_root"
