#!/usr/bin/env bash
set -euo pipefail
OPENVELA_ROOT=${OPENVELA_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}

cd "${AI_AGENT_TEST_ROOT:-${OPENVELA_ROOT}/packages/ai_agent}"
CJSON=${OPENVELA_ROOT}/apps/netutils/cjson/cJSON
MBEDTLS=${OPENVELA_ROOT}/apps/crypto/mbedtls/mbedtls/include

python3 tests/recording_signal_test.py

gcc -std=gnu11 -Wall -Wextra -Werror \
  -DLABTWIN_DASHBOARD_HOST_TEST \
  -Iinclude -Isrc -I"$CJSON" \
  tests/labtwin_dashboard_host_test.c \
  src/labtwin/labtwin_dashboard.c "$CJSON/cJSON.c" \
  -lpthread -lm -o /tmp/labtwin_dashboard_host_test
/tmp/labtwin_dashboard_host_test

gcc -std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -DOK=0 -DERROR=-1 \
  -Darc4random_buf=test_arc4random_buf -include tests/admin_auth_host_compat.h \
  -DAUTH_LEGACY_FILE_PATH='"/tmp/labtwin-admin-auth-legacy-test.json"' \
  -Iinclude -Isrc -I"$CJSON" -I"$MBEDTLS" \
  tests/admin_auth_host_test.c \
  src/infra/admin_auth.c "$CJSON/cJSON.c" \
  -lpthread -lm -o /tmp/admin_auth_host_test
/tmp/admin_auth_host_test
/tmp/admin_auth_host_test --legacy

bash ./tools/Run-M4HostTests.sh

memory_root=/tmp/ai-agent-structured-memory-test
if [[ "$memory_root" == /tmp/ai-agent-structured-memory-test ]]; then
  rm -rf -- "$memory_root"
fi

gcc -std=gnu11 -Wall -Wextra -Werror -DOK=0 -DERROR=-1 \
  -DSTRUCTURED_MEMORY_HOST_TEST \
  -DSTRUCTURED_MEMORY_TEST_ROOT='"/tmp/ai-agent-structured-memory-test"' \
  -Iinclude -Isrc -I"$CJSON" \
  tests/structured_memory_host_test.c src/core/structured_memory.c \
  "$CJSON/cJSON.c" -lpthread -lm -o /tmp/structured_memory_host_test
/tmp/structured_memory_host_test

report_root=/tmp/labtwin-report-test
if [[ "$report_root" == /tmp/labtwin-report-test ]]; then
  rm -rf -- "$report_root"
fi

gcc -std=gnu11 -Wall -Wextra -Werror -DOK=0 -DERROR=-1 \
  -DLABTWIN_HOST_TEST -DLABTWIN_ENV_HOST_TEST -DLABTWIN_REPORT_HOST_TEST \
  -DLABTWIN_ROOT='"/tmp/labtwin-report-test"' \
  -DLABTWIN_REPORT_ROOT='"/tmp/labtwin-report-test/reports"' \
  -Iinclude -Isrc -I"$CJSON" \
  tests/labtwin_report_host_test.c src/labtwin/labtwin_report.c \
  src/labtwin/labtwin_service.c src/labtwin/labtwin_environment.c \
  "$CJSON/cJSON.c" -lpthread -lm -o /tmp/labtwin_report_host_test
/tmp/labtwin_report_host_test

session_root=/tmp/ai-agent-session-lifecycle-test
if [[ "$session_root" == /tmp/ai-agent-session-lifecycle-test ]]; then
  rm -rf -- "$session_root"
fi

gcc -std=gnu11 -Wall -Wextra -Werror -DOK=0 -DERROR=-1 -DAGENT_HOST_TEST \
  -DCONFIG_EXAMPLES_AI_AGENT_VELA_DATA_DIR='"/tmp/ai-agent-session-lifecycle-test"' \
  -Iinclude -Isrc -I"$CJSON" \
  tests/agent_runtime_lifecycle_host_test.c src/core/session_mgr.c \
  src/core/message_bus.c "$CJSON/cJSON.c" -lpthread -lm \
  -o /tmp/agent_runtime_lifecycle_host_test
/tmp/agent_runtime_lifecycle_host_test
