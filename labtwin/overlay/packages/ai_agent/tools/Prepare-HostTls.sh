#!/usr/bin/env bash
set -euo pipefail
OPENVELA_ROOT=${OPENVELA_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}
MBEDTLS=${MBEDTLS_SOURCE:-${OPENVELA_ROOT}/apps/crypto/mbedtls/mbedtls}
HOST_LIB=${MBEDTLS_HOST_LIB:-/tmp/mbedtls-host-task-20261002-02/library}
if [[ ! -f "$HOST_LIB/libmbedcrypto.a" || ! -f "$HOST_LIB/libmbedx509.a" || ! -f "$HOST_LIB/libmbedtls.a" ]]; then
  cmake -S "$MBEDTLS" -B "${HOST_LIB%/library}" -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF -DCMAKE_C_FLAGS=-Wno-error=missing-prototypes
  cmake --build "${HOST_LIB%/library}" -j4
fi
export MBEDTLS HOST_LIB
