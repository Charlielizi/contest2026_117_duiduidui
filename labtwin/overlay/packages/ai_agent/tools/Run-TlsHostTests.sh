#!/usr/bin/env bash
set -euo pipefail
OPENVELA_ROOT=${OPENVELA_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}
cd "${AI_AGENT_TEST_ROOT:-${OPENVELA_ROOT}/packages/ai_agent}"
tls_root=$(mktemp -d /tmp/labtwin-tls-XXXXXX)
source tools/Prepare-HostTls.sh
gcc -std=gnu11 -Wall -Wextra -Werror -Iinclude -Isrc -I"$MBEDTLS/include" \
  tests/tls_trust_host_test.c "$HOST_LIB/libmbedtls.a" "$HOST_LIB/libmbedx509.a" \
  "$HOST_LIB/libmbedcrypto.a" -lpthread -lm -o "$tls_root/tls-test"
openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj /CN=localhost \
  -addext subjectAltName=DNS:localhost -keyout "$tls_root/key.pem" -out "$tls_root/valid.pem" >/dev/null 2>&1
openssl req -new -key "$tls_root/key.pem" -subj /CN=localhost -out "$tls_root/request.pem" >/dev/null 2>&1
openssl x509 -req -in "$tls_root/request.pem" -signkey "$tls_root/key.pem" -days -1 -out "$tls_root/expired.pem" >/dev/null 2>&1
port=29443
server_pid=
cleanup() { if [[ -n "$server_pid" ]]; then kill "$server_pid" 2>/dev/null || true; wait "$server_pid" 2>/dev/null || true; fi; }
trap cleanup EXIT
openssl s_server -accept "127.0.0.1:$port" -cert "$tls_root/valid.pem" -key "$tls_root/key.pem" -tls1_2 -quiet >"$tls_root/server.log" 2>&1 &
server_pid=$!
sleep 1
"$tls_root/tls-test" "$tls_root/valid.pem" localhost "$port" 1
"$tls_root/tls-test" "$tls_root/valid.pem" wrong.example "$port" 0
"$tls_root/tls-test" "${TLS_TEST_CA:-${OPENVELA_ROOT}/vendor/allwinnertech/lichee/board/common/data/res/etc/ssl/curl/ca-certificates.crt}" localhost "$port" 0
"$tls_root/tls-test" "$tls_root/missing.pem" localhost "$port" 2
"$tls_root/tls-test" tools/Run-TlsHostTests.sh localhost "$port" 2
cleanup; server_pid=
openssl s_server -accept "127.0.0.1:$port" -cert "$tls_root/expired.pem" -key "$tls_root/key.pem" -tls1_2 -quiet >"$tls_root/expired.log" 2>&1 &
server_pid=$!
sleep 1
"$tls_root/tls-test" "$tls_root/expired.pem" localhost "$port" 0
