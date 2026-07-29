#!/usr/bin/env bash
#
# watch-observer.sh - highlight the ESP32-C3 observer's console.
#
# Makes a received wearable beacon obvious against the IDF boot log: incidents
# are printed as a banner, heartbeats as a single dim line so liveness is
# visible without drowning the incidents. Reads stdin, writes stdout:
#
#   idf.py -p /dev/ttyACM1 monitor | watch-observer.sh
#
# Matches the "obs dev=.. rssi=.. evt=.." records emitted by mind_uplink (mesh
# mode) and main.c's drain task (CONFIG_MIND_BLE_ONLY). Event numbering comes
# from shared/schema.h.

set -uo pipefail

BOLD=$'\033[1m'; DIM=$'\033[2m'; RST=$'\033[0m'
RED=$'\033[31m'; GRN=$'\033[32m'; YEL=$'\033[33m'; CYA=$'\033[36m'; MAG=$'\033[35m'

label_for() {
    case "$1" in
        0) printf 'HEARTBEAT' ;;
        1) printf 'MOTION' ;;
        2) printf 'POSSIBLE FALL' ;;
        3) printf 'CONFIRMED FALL' ;;
        4) printf 'POSSIBLE DISTRESS (shout)' ;;
        5) printf 'FALL + SHOUT' ;;
        *) printf 'UNKNOWN(%s)' "$1" ;;
    esac
}

colour_for() {
    case "$1" in
        2) printf '%s' "$YEL" ;;
        3) printf '%s' "$RED" ;;
        4) printf '%s' "$MAG" ;;
        5) printf '%s' "$RED$BOLD" ;;
        *) printf '%s' "$CYA" ;;
    esac
}

hb=0
while IFS= read -r line; do
    case "$line" in
        *obs\ dev=*)
            evt=$(printf '%s' "$line" | sed -n 's/.*evt=\([0-9]*\).*/\1/p')
            dev=$(printf '%s' "$line" | sed -n 's/.*dev=\([0-9]*\).*/\1/p')
            rssi=$(printf '%s' "$line" | sed -n 's/.*rssi=\(-\?[0-9]*\).*/\1/p')
            conf=$(printf '%s' "$line" | sed -n 's/.*conf=\([0-9]*\).*/\1/p')
            svm=$(printf '%s' "$line" | sed -n 's/.*svm=\([0-9]*\).*/\1/p')
            mic=$(printf '%s' "$line" | sed -n 's/.*mic=\([0-9]*\).*/\1/p')
            seq=$(printf '%s' "$line" | sed -n 's/.*seq=\([0-9]*\).*/\1/p')

            if [ "${evt:-0}" = "0" ]; then
                hb=$((hb + 1))
                printf '%s\n' "${DIM}  heartbeat #${hb}  dev=${dev} rssi=${rssi} dBm  svm=${svm} mg  seq=${seq}${RST}"
            else
                c=$(colour_for "$evt")
                printf '%s\n' "${c}############ BEACON RECEIVED ############${RST}"
                printf '%s\n' "${c}${BOLD}  $(label_for "$evt")  (type=${evt})${RST}"
                printf '%s\n' "${c}  dev=${dev}  rssi=${rssi} dBm  conf=${conf}%${RST}"
                printf '%s\n' "${c}  svm=${svm} mg  mic=${mic}  seq=${seq}${RST}"
                printf '%s\n' "${c}########################################${RST}"
            fi
            ;;
        *observer\ scanning*|*sidecar\ boot*|*sidecar\ running*|*BLE-only*)
            printf '%s\n' "${GRN}${BOLD}${line}${RST}"
            ;;
        *E\ \(*|*error*|*ERROR*)
            printf '%s\n' "${RED}${line}${RST}"
            ;;
        *)
            printf '%s\n' "${DIM}${line}${RST}"
            ;;
    esac
done
