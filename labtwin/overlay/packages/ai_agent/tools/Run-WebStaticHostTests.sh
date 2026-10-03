#!/usr/bin/env bash
set -euo pipefail
task_root=$(cd "$(dirname "$0")/.." && pwd)
task_temp=$(mktemp -d /tmp/labtwin-web-static.XXXXXX)
trap 'rm -rf -- "$task_temp"' EXIT
mkdir "$task_temp/root"
cc -std=c11 -Wall -Wextra -Werror -I"$task_root/src" \
  -DWEB_ROOT="\"$task_temp/root\"" \
  "$task_root/src/infra/web_static.c" "$task_root/tests/web_static_host_test.c" \
  -o "$task_temp/test"
"$task_temp/test"
