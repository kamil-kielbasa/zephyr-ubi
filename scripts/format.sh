#!/bin/bash
# Copyright (c) 2026 Kamil Kiełbasa
# SPDX-License-Identifier: MIT
#
# Formats every C file git lists (tracked and new, not ignored) with .clang-format.

set -e

cd "$(dirname "$0")/.."

git ls-files -z --cached --others --exclude-standard -- '*.c' '*.h' |
	xargs -0 -r clang-format -i
