#!/usr/bin/env bash
#
# watch-wearable.sh - highlight the wearable's serial stream.
#
# Colourises the micro:bit console so an incident stands out against the 1 Hz
# accelerometer chatter: incidents are printed as a banner, shouts in yellow,
# and routine samples dimmed. Reads stdin, writes stdout, so it composes with
# grabserial or any other reader:
#
#   grabserial -d /dev/ttyACM0 -b 115200 -t | watch-wearable.sh
#
# Event types come from shared/schema.h (mind_event_type).

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

while IFS= read -r line; do
    case "$line" in
        *EVENT\ type=*)
            evt=$(printf '%s' "$line" | sed -n 's/.*EVENT type=\([0-9]*\).*/\1/p')
            conf=$(printf '%s' "$line" | sed -n 's/.*conf=\([0-9]*\).*/\1/p')
            svm=$(printf '%s' "$line" | sed -n 's/.*svm=\([0-9]*\).*/\1/p')
            mic=$(printf '%s' "$line" | sed -n 's/.*mic=\([0-9]*\).*/\1/p')
            seq=$(printf '%s' "$line" | sed -n 's/.*seq=\([0-9]*\).*/\1/p')
            c=$(colour_for "$evt")
            printf '%s\n' "${c}================ WEARABLE EVENT ================${RST}"
            printf '%s\n' "${c}${BOLD}  $(label_for "$evt")  (type=${evt})${RST}"
            printf '%s\n' "${c}  confidence=${conf}%  svm=${svm} mg  mic=${mic}  seq=${seq}${RST}"
            printf '%s\n' "${c}===============================================${RST}"
            ;;
        *shout\ detected*)
            printf '%s\n' "${YEL}${BOLD}>>> SHOUT DETECTED${RST} ${DIM}${line}${RST}"
            ;;
        *IMU:*|*detection\ running*|*microT-Kernel*)
            printf '%s\n' "${GRN}${line}${RST}"
            ;;
        *)
            printf '%s\n' "${DIM}${line}${RST}"
            ;;
    esac
done
