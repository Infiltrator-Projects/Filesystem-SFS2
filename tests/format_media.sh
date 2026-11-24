#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
"$1" "$2" "$work/512.img" 4096 512
"$1" "$2" "$work/4096.img" 4096 4096
