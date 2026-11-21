#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
carrier="${1:?missing SFS2.so carrier}"
elf="${2:?missing freestanding SFS2.so}"
test -s "$carrier" && test -s "$elf"
nm -D --defined-only "$elf" | grep -F infiltrator_filesystem_api_v2
nm -D --defined-only "$carrier" | grep -F infiltrator_filesystem_api_v2
! nm -D --defined-only "$elf" | grep -E 'infiltrator_filesystem_.*_v1|infiltrator_filesystem_module_v[345]'
! readelf -d "$elf" | grep '(NEEDED)'
readelf -SW "$carrier" | grep -F .infiltrator_boot
temp="$(mktemp -d)"
trap 'rm -rf "$temp"' EXIT
objcopy --dump-section ".infiltrator_boot=$temp/actual.elf" "$carrier"
cmp "$elf" "$temp/actual.elf"
echo 'SFS2_SOURCE_IDENTICAL_FREESTANDING_COMMON_V2_CARRIER_PASS'
