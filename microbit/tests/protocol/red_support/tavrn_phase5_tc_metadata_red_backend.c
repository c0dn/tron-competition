/* Intentional RED fallback: no metadata or TC-maintenance policy exists here. */
#include "tavrn_phase5_tc_metadata_contract.h"

#include <string.h>

#if defined(TAVRN_PHASE5_TC_METADATA_RED_MODE)

static tavrn_tc_metadata_status_t unavailable(void)
{
    return TAVRN_TC_METADATA_UNAVAILABLE;
}

tavrn_tc_metadata_status_t phase5_tc_metadata_red_init(
    tavrn_tc_metadata_state_t *state, const tavrn_tc_metadata_config_t *config)
{
    (void)config;
    if (state != NULL) memset(state, 0, sizeof(*state));
    return unavailable();
}

tavrn_tc_metadata_status_t phase5_tc_metadata_red_sequence_prepare(
    tavrn_tc_metadata_state_t *state, tavrn_tc_event_t event,
    tavrn_tc_sequence_ticket_t *ticket)
{
    (void)state; (void)event;
    if (ticket != NULL) memset(ticket, 0, sizeof(*ticket));
    return unavailable();
}

tavrn_tc_metadata_status_t phase5_tc_metadata_red_sequence_commit(
    tavrn_tc_metadata_state_t *state, const tavrn_tc_sequence_ticket_t *ticket,
    tavrn_tc_admission_t admission, uint32_t now_ms)
{
    (void)state; (void)ticket; (void)admission; (void)now_ms;
    return unavailable();
}

tavrn_tc_metadata_status_t phase5_tc_metadata_red_prepare_origin(
    tavrn_tc_metadata_state_t *state, tavrn_tc_origin_cause_t cause,
    const tavrn_adva_t *subject, const tavrn_adva_t *hop,
    const tavrn_adva_t *destination, uint32_t now_ms,
    tavrn_tc_metadata_action_t *action)
{
    (void)state; (void)cause; (void)subject; (void)hop; (void)destination; (void)now_ms;
    if (action != NULL) memset(action, 0, sizeof(*action));
    return unavailable();
}

tavrn_tc_metadata_status_t phase5_tc_metadata_red_commit_origin(
    tavrn_tc_metadata_state_t *state, const tavrn_tc_metadata_action_t *action,
    tavrn_tc_admission_t admission, uint32_t now_ms)
{
    (void)state; (void)action; (void)admission; (void)now_ms;
    return unavailable();
}

tavrn_tc_metadata_status_t phase5_tc_metadata_red_receive(
    tavrn_tc_metadata_state_t *state, const uint8_t pdu[24], uint8_t len,
    const tavrn_adva_t *outer, uint32_t now_ms, tavrn_tc_metadata_action_t *relay)
{
    (void)state; (void)pdu; (void)len; (void)outer; (void)now_ms;
    if (relay != NULL) memset(relay, 0, sizeof(*relay));
    return unavailable();
}

tavrn_tc_metadata_status_t phase5_tc_metadata_red_commit_relay(
    tavrn_tc_metadata_state_t *state, const tavrn_tc_metadata_action_t *action,
    tavrn_tc_admission_t admission, uint32_t now_ms)
{
    (void)state; (void)action; (void)admission; (void)now_ms;
    return unavailable();
}

tavrn_tc_metadata_status_t phase5_tc_metadata_red_create(
    tavrn_tc_metadata_state_t *state, const tavrn_metadata_candidate_t *candidate,
    uint32_t now_ms)
{
    (void)state; (void)candidate; (void)now_ms;
    return unavailable();
}

tavrn_tc_metadata_status_t phase5_tc_metadata_red_select(
    const tavrn_tc_metadata_state_t *state, tavrn_metadata_frame_kind_t kind,
    uint8_t unreachable, uint32_t now_ms, tavrn_metadata_selection_t *selection)
{
    (void)state; (void)kind; (void)unreachable; (void)now_ms;
    if (selection != NULL) memset(selection, 0, sizeof(*selection));
    return unavailable();
}

tavrn_tc_metadata_status_t phase5_tc_metadata_red_commit(
    tavrn_tc_metadata_state_t *state, const tavrn_metadata_selection_t *selection,
    tavrn_tc_admission_t admission, uint32_t now_ms)
{
    (void)state; (void)selection; (void)admission; (void)now_ms;
    return unavailable();
}

tavrn_tc_metadata_status_t phase5_tc_metadata_red_attach_control(
    const tavrn_validated_control_t *base, const tavrn_metadata_selection_t *selection,
    tavrn_metadata_attached_control_t *attached)
{
    (void)base; (void)selection;
    if (attached != NULL) memset(attached, 0, sizeof(*attached));
    return unavailable();
}

tavrn_tc_metadata_status_t phase5_tc_metadata_red_receive_metadata(
    tavrn_tc_metadata_state_t *state, const tavrn_metadata_rx_t *rx, uint32_t now_ms)
{
    (void)state; (void)rx; (void)now_ms;
    return unavailable();
}

tavrn_tc_metadata_status_t phase5_tc_metadata_red_receive_control(
    tavrn_tc_metadata_state_t *state, const tavrn_rx_control_event_t *event,
    uint32_t now_ms)
{
    (void)state; (void)event; (void)now_ms;
    return unavailable();
}

tavrn_tc_metadata_status_t phase5_tc_metadata_red_snapshot(
    const tavrn_tc_metadata_state_t *state, tavrn_tc_metadata_snapshot_t *snapshot)
{
    (void)state;
    if (snapshot != NULL) memset(snapshot, 0, sizeof(*snapshot));
    return unavailable();
}

#endif
