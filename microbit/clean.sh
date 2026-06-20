#!/usr/bin/env bash

set -euo pipefail

source "$(cd "$(dirname "$0")" && pwd)/scripts/common.sh"

rm -rf "$DEFAULT_BUILD_DIR"
printf 'Removed build directory: %s\n' "$DEFAULT_BUILD_DIR"
