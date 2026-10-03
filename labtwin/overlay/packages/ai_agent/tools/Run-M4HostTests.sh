#!/usr/bin/env bash
set -euo pipefail
OPENVELA_ROOT=${OPENVELA_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}

cd "${AI_AGENT_TEST_ROOT:-${OPENVELA_ROOT}/packages/ai_agent}"
CJSON=${OPENVELA_ROOT}/apps/netutils/cjson/cJSON

protocol_root=/tmp/labtwin-protocol-v1-test
if [[ "$protocol_root" == /tmp/labtwin-protocol-v1-test ]]; then
  rm -rf -- "$protocol_root"
fi

gcc -std=gnu11 -Wall -Wextra -Werror -DLABTWIN_PROTOCOL_HOST_TEST \
  -Iinclude -Isrc -I"$CJSON" \
  tests/labtwin_protocol_host_test.c \
  src/labtwin/labtwin_protocol.c "$CJSON/cJSON.c" \
  -lpthread -lm -o /tmp/labtwin_protocol_host_test
/tmp/labtwin_protocol_host_test

recording_root=/tmp/labtwin-recordings-host
if [[ "$recording_root" == /tmp/labtwin-recordings-host ]]; then
  rm -rf -- "$recording_root"
fi

gcc -std=gnu11 -Wall -Wextra -Werror -DRECORDING_HOST_TEST \
  '-DRECORDINGS_PARENT="/tmp/labtwin-recordings-host"' \
  '-DRECORDINGS_ROOT="/tmp/labtwin-recordings-host/recordings"' \
  -Iinclude -Isrc -I"$CJSON" \
  tests/recording_service_host_test.c src/voice/recording_service.c \
  "$CJSON/cJSON.c" -lpthread -lm -o /tmp/recording_service_host_test
/tmp/recording_service_host_test

controller_root=/tmp/labtwin-controller-v1-test
if [[ "$controller_root" == /tmp/labtwin-controller-v1-test ]]; then
  rm -rf -- "$controller_root"
fi

gcc -std=gnu11 -Wall -Wextra -Werror \
  -DLABTWIN_HOST_TEST -DLABTWIN_ENV_HOST_TEST -DLABTWIN_PROTOCOL_HOST_TEST -DLABTWIN_CONTROLLER_HOST_TEST \
  -DLABTWIN_ROOT=\"/tmp/labtwin-controller-v1-test\" \
  -Iinclude -Isrc -I"$CJSON" \
  tests/labtwin_controller_host_test.c \
  src/labtwin/labtwin_controller.c src/labtwin/labtwin_protocol.c \
  src/labtwin/labtwin_service.c src/labtwin/labtwin_environment.c \
  "$CJSON/cJSON.c" -lpthread -lm -o /tmp/labtwin_controller_host_test
/tmp/labtwin_controller_host_test

gcc -std=gnu11 -DLABTWIN_ENV_HOST_TEST \
  -DCONFIG_AI_AGENT_LABTWIN_ENV_TEST \
  -Iinclude -Isrc -I"$CJSON" \
  tests/labtwin_environment_host_test.c \
  src/labtwin/labtwin_environment.c "$CJSON/cJSON.c" \
  -lpthread -lm -o /tmp/labtwin_environment_host_test
/tmp/labtwin_environment_host_test

test_root=/tmp/labtwin-m2-test-20260714-v2
if [[ "$test_root" == /tmp/labtwin-m2-test-20260714-v2 ]]; then
  rm -rf -- "$test_root"
fi

gcc -std=gnu11 -DLABTWIN_HOST_TEST -DLABTWIN_ENV_HOST_TEST \
  -Iinclude -Isrc -I"$CJSON" \
  tests/labtwin_host_test.c src/labtwin/labtwin_service.c \
  src/labtwin/labtwin_environment.c "$CJSON/cJSON.c" \
  -lpthread -lm -o /tmp/labtwin_host_test
/tmp/labtwin_host_test

gcc -std=gnu11 -DLABTWIN_HOST_TEST -DLABTWIN_ENV_HOST_TEST \
  -Iinclude -Isrc -I"$CJSON" \
  tests/labtwin_recovery_test.c src/labtwin/labtwin_service.c \
  src/labtwin/labtwin_environment.c "$CJSON/cJSON.c" \
  -lpthread -lm -o /tmp/labtwin_recovery_test
/tmp/labtwin_recovery_test
