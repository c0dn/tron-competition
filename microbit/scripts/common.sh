#!/usr/bin/env bash

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEFAULT_BUILD_DIR="${REPO_ROOT}/build"
DEFAULT_TARGET="test_firmware"
TOOLCHAIN_FILE="${REPO_ROOT}/cmake/arm-none-eabi-gcc.cmake"

find_tool() {
    local tool="$1"

    if command -v "$tool" >/dev/null 2>&1; then
        command -v "$tool"
        return 0
    fi

    if [[ -x "${HOME}/.local/bin/${tool}" ]]; then
        printf '%s\n' "${HOME}/.local/bin/${tool}"
        return 0
    fi

    printf 'Required tool not found: %s\n' "$tool" >&2
    return 1
}

configure_build() {
    local build_dir="$1"
    local target="${2:-ble_mesh_node}"
    local phase1_target="LEGACY"
    local node_mode="LEGACY_FLOOD"

    case "$target" in
        ble_mesh_node) phase1_target="LEGACY" ;;
        ble_link_v2_testbed) phase1_target="LINK"; node_mode="NOT_APPLICABLE" ;;
    esac
    cmake -S "${REPO_ROOT}" -B "$build_dir" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
        -DTRON_PHASE1_TARGET="$phase1_target" \
        -DTRON_NODE_MODE="$node_mode"
}

firmware_output_dir() {
    local build_dir="$1"
    local target="$2"
    printf '%s/firmware/%s\n' "$build_dir" "$target"
}

firmware_elf() {
    local build_dir="$1"
    local target="$2"
    printf '%s/%s.elf\n' "$(firmware_output_dir "$build_dir" "$target")" "$target"
}

serial_by_id_links() {
    local link

    if [[ ! -d /dev/serial/by-id ]]; then
        return 0
    fi

    for link in /dev/serial/by-id/*; do
        [[ -L "$link" ]] || continue

        case "$(readlink -f "$link")" in
            /dev/ttyACM*|/dev/ttyUSB*)
                printf '%s\n' "$link"
                ;;
        esac
    done
}

serial_device_from_link() {
    local link="$1"
    readlink -f "$link"
}

device_uid_from_link() {
    local base
    base="$(basename "$1")"

    if [[ "$base" =~ _([^_]+)-if[0-9]+$ ]]; then
        printf '%s\n' "${BASH_REMATCH[1]}"
        return 0
    fi

    printf '%s\n' "$base"
}

serial_device_for_uid() {
    local uid="$1"
    local link
    local matches=()
    local target
    local -A seen=()

    while IFS= read -r link; do
        [[ "$(basename "$link")" == *"$uid"* ]] || continue

        target="$(serial_device_from_link "$link")"
        if [[ -z "${seen[$target]:-}" ]]; then
            matches+=("$target")
            seen[$target]=1
        fi
    done < <(serial_by_id_links)

    case "${#matches[@]}" in
        1)
            printf '%s\n' "${matches[0]}"
            ;;
        0)
            printf 'No serial device matched UID: %s\n' "$uid" >&2
            return 1
            ;;
        *)
            printf 'UID matched multiple serial devices: %s\n' "$uid" >&2
            printf 'Matches:\n' >&2
            printf '  %s\n' "${matches[@]}" >&2
            return 1
            ;;
    esac
}

device_uid_for_serial_device() {
    local device="$1"
    local normalized_device="$device"
    local link
    local target
    local matches=()

    if [[ -e "$device" ]]; then
        normalized_device="$(readlink -f "$device")"
    fi

    while IFS= read -r link; do
        target="$(serial_device_from_link "$link")"
        [[ "$target" == "$normalized_device" ]] || continue
        matches+=("$(device_uid_from_link "$link")")
    done < <(serial_by_id_links)

    case "${#matches[@]}" in
        1)
            printf '%s\n' "${matches[0]}"
            ;;
        0)
            printf 'No probe UID matched serial device: %s\n' "$device" >&2
            return 1
            ;;
        *)
            printf 'Serial device matched multiple probe UIDs: %s\n' "$device" >&2
            printf 'Matches:\n' >&2
            printf '  %s\n' "${matches[@]}" >&2
            return 1
            ;;
    esac
}

select_serial_link() {
    local links=()
    local link
    local index=1
    local selection

    while IFS= read -r link; do
        links+=("$link")
    done < <(serial_by_id_links)

    case "${#links[@]}" in
        0)
            printf 'No /dev/serial/by-id entries found for ttyACM*/ttyUSB* devices.\n' >&2
            return 1
            ;;
        1)
            printf '%s\n' "${links[0]}"
            return 0
            ;;
    esac

    printf 'Select a serial device:\n' >&2
    for link in "${links[@]}"; do
        printf '  %d) %s -> %s\n' \
            "$index" \
            "$(device_uid_from_link "$link")" \
            "$(serial_device_from_link "$link")" >&2
        ((index++))
    done

    while true; do
        printf 'Enter choice [1-%d]: ' "${#links[@]}" >&2
        IFS= read -r selection || return 1

        if [[ "$selection" =~ ^[0-9]+$ ]] && (( selection >= 1 && selection <= ${#links[@]} )); then
            printf '%s\n' "${links[selection-1]}"
            return 0
        fi

        printf 'Invalid selection: %s\n' "$selection" >&2
    done
}
