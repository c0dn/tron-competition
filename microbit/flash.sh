#!/usr/bin/env bash

set -euo pipefail

source "$(cd "$(dirname "$0")" && pwd)/scripts/common.sh"

target="$DEFAULT_TARGET"
build_dir="$DEFAULT_BUILD_DIR"
device=""
uid=""
baud="115200"
monitor_after_flash="0"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --device|-d)
            device="${2:?Missing value for $1}"
            shift 2
            ;;
        --uid|--probe|-u)
            uid="${2:?Missing value for $1}"
            shift 2
            ;;
        --baud)
            baud="${2:?Missing value for $1}"
            shift 2
            ;;
        --monitor|-m)
            monitor_after_flash="1"
            shift
            ;;
        --help|-h)
            cat <<'EOF'
Usage: ./flash.sh [target] [--uid <probe-id>] [--device /dev/ttyACM0] [--baud 115200] [--monitor]

Examples:
  ./flash.sh
  ./flash.sh test_firmware
  ./flash.sh ble_observer --uid 9906360200052820aedc48cd54eaf396000000006e052820 --monitor
  ./flash.sh test_firmware --device /dev/ttyACM0 --monitor
EOF
            exit 0
            ;;
        *)
            if [[ "$target" == "$DEFAULT_TARGET" ]]; then
                target="$1"
                shift
            else
                printf 'Unexpected argument: %s\n' "$1" >&2
                exit 1
            fi
            ;;
    esac
done

pyocd_bin="$(find_tool pyocd)"
pyocd_args=()

if [[ -z "$uid" && -n "$device" ]]; then
    uid="$(device_uid_for_serial_device "$device" 2>/dev/null || true)"
fi

if [[ "$monitor_after_flash" == "1" && -z "$uid" && -z "$device" ]]; then
    selected_link="$(select_serial_link)"
    uid="$(device_uid_from_link "$selected_link")"
    device="$(serial_device_from_link "$selected_link")"
elif [[ -n "$uid" && -z "$device" ]]; then
    if [[ "$monitor_after_flash" == "1" ]]; then
        device="$(serial_device_for_uid "$uid")"
    else
        device="$(serial_device_for_uid "$uid" 2>/dev/null || true)"
    fi
fi

if [[ -n "$uid" ]]; then
    pyocd_args+=(--uid "$uid")
fi

configure_build "$build_dir"
cmake --build "$build_dir" --target "$target" --parallel

elf_path="$(firmware_elf "$build_dir" "$target")"

"$pyocd_bin" erase --mass "${pyocd_args[@]}"
"$pyocd_bin" load "$elf_path" "${pyocd_args[@]}"
"$pyocd_bin" reset "${pyocd_args[@]}"

printf 'Flashed target: %s\n' "$target"
printf 'ELF: %s\n' "$elf_path"

if [[ -n "$uid" ]]; then
    printf 'Probe UID: %s\n' "$uid"
fi

if [[ -n "$device" ]]; then
    printf 'Device: %s\n' "$device"
    printf 'Baud: %s\n' "$baud"
fi

if [[ "$monitor_after_flash" == "1" ]]; then
    if [[ -z "$device" ]]; then
        printf '--monitor requires a detected serial device.\n' >&2
        exit 1
    fi

    grabserial_bin="$(find_tool grabserial)"
    exec "$grabserial_bin" -d "$device" -b "$baud" -t
fi
