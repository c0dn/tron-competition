#!/usr/bin/env bash

set -euo pipefail

source "$(cd "$(dirname "$0")" && pwd)/scripts/common.sh"

target="${1:-$DEFAULT_TARGET}"
build_dir="$DEFAULT_BUILD_DIR"

configure_build "$build_dir" "$target"
cmake --build "$build_dir" --target "$target" --parallel

printf 'Built target: %s\n' "$target"
printf 'ELF: %s\n' "$(firmware_elf "$build_dir" "$target")"
