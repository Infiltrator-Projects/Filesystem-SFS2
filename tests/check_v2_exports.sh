#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
so=${1:?SFS2.so required}
test -s "$so"
test "$(nm -D --defined-only "$so" |
  awk '$3=="infiltrator_filesystem_api_v2"{n++}END{print n+0}')" -eq 1
unexpected="$(nm -D --defined-only "$so" | awk '{print $3}' |
  sed -E 's/@@?.*$//' |
  grep -v '^infiltrator_filesystem_api_v2$' || true)"
test -z "$unexpected" || { echo "Unexpected exported SFS2 runtime functions: $unexpected" >&2;exit 1; }
