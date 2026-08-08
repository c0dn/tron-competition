#ifndef TAVRN_PHASE4_ESC_MENTOR_CONTRACT_H
#define TAVRN_PHASE4_ESC_MENTOR_CONTRACT_H

/*
 * Frozen Step 5.8 public seam.  Production owns the implementation in
 * tavrn_esc.{h,c}, tavrn_mentorship.{h,c}, and the FULL/router binding; this
 * test-only header declares the exact API until those headers exist.
 *
 * All functions are by-value/fixed-capacity.  They are intentionally the
 * smallest surface which lets the host test observe the required GTT context,
 * immutable snapshot, bootstrap gate, and width handover without reaching
 * around the real wire -> link -> router path.
 */
#include <stdint.h>

#include "aodv_core.h"
#include "tavrn_gtt.h"
#include "tavrn_router.h"

#ifdef TAVRN_ESC_MENTORSHIP_API

#include "tavrn_esc.h"
#include "tavrn_mentorship.h"

#else

#define TAVRN_ESC_FIXED_K 1u
#define TAVRN_MENTORSHIP_SNAPSHOT_CAPACITY TAVRN_GTT_CAPACITY
#define TAVRN_MENTORSHIP_OFFER_CAPACITY 8u
#define TAVRN_MENTORSHIP_JOIN_OBLIGATION_CAPACITY 3u

typedef char tavrn_phase4_fixed_k_guard[
    (TAVRN_ESC_FIXED_K == 1u) ? 1 : -1];
typedef char tavrn_phase4_snapshot_capacity_guard[
    (TAVRN_MENTORSHIP_SNAPSHOT_CAPACITY == TAVRN_GTT_CAPACITY) ? 1 : -1];
typedef char tavrn_phase4_offer_capacity_guard[
    (TAVRN_MENTORSHIP_OFFER_CAPACITY == 8u) ? 1 : -1];
typedef char tavrn_phase4_join_obligation_capacity_guard[
    (TAVRN_MENTORSHIP_JOIN_OBLIGATION_CAPACITY == 3u) ? 1 : -1];

typedef enum tavrn_esc_context_status {
    TAVRN_ESC_CONTEXT_UNIQUE = 0,
    TAVRN_ESC_CONTEXT_UNKNOWN,
    TAVRN_ESC_CONTEXT_RESERVED,
    TAVRN_ESC_CONTEXT_COLLIDING,
    TAVRN_ESC_CONTEXT_INVALID,
} tavrn_esc_context_status_t;

typedef struct tavrn_esc_context_match {
    tavrn_adva_t identity;
    uint8_t sid8;
} tavrn_esc_context_match_t;

typedef struct tavrn_mentorship_record {
    tavrn_adva_t identity;
    uint16_t serial;
    uint8_t serial_present;
    uint8_t departed;
    uint8_t ttl_bucket;
    uint8_t hop_count;
} tavrn_mentorship_record_t;

typedef struct tavrn_mentorship_snapshot_data {
    uint16_t snapshot_id;
    uint8_t count;
    tavrn_mentorship_record_t records[TAVRN_MENTORSHIP_SNAPSHOT_CAPACITY];
} tavrn_mentorship_snapshot_data_t;

typedef struct tavrn_mentorship_offer {
    tavrn_adva_t mentor;
    tavrn_adva_t mentee;
    uint16_t snapshot_id;
    uint16_t boot_nonce;
    uint8_t snapshot_count;
    uint8_t rssi_magnitude_db;
} tavrn_mentorship_offer_t;

typedef enum tavrn_mentorship_offer_status {
    TAVRN_MENTORSHIP_OFFER_ACCEPTED = 0,
    TAVRN_MENTORSHIP_OFFER_DUPLICATE,
    TAVRN_MENTORSHIP_OFFER_SUPPRESSED,
    TAVRN_MENTORSHIP_OFFER_INVALID,
    TAVRN_MENTORSHIP_OFFER_STORAGE_FULL,
} tavrn_mentorship_offer_status_t;

typedef enum tavrn_mentorship_state {
    TAVRN_MENTORSHIP_REJOINING = 0,
    TAVRN_MENTORSHIP_COLLECTING_OFFERS,
    TAVRN_MENTORSHIP_SYNCING,
    TAVRN_MENTORSHIP_SID8_ACTIVE,
    TAVRN_MENTORSHIP_IDENTITY_CONFLICT,
} tavrn_mentorship_state_t;

typedef enum tavrn_mentorship_status {
    TAVRN_MENTORSHIP_OK = 0,
    TAVRN_MENTORSHIP_BOOTSTRAP_ADMITTED,
    TAVRN_MENTORSHIP_SYNC_PAGE_DUPLICATE,
    TAVRN_MENTORSHIP_RESTARTED,
    TAVRN_MENTORSHIP_SELF_BOOTSTRAPPED,
    TAVRN_MENTORSHIP_GATED,
    TAVRN_MENTORSHIP_UNRESOLVED,
    TAVRN_MENTORSHIP_COLLISION,
    TAVRN_MENTORSHIP_BUSY,
    TAVRN_MENTORSHIP_INVALID,
    TAVRN_MENTORSHIP_UNIMPLEMENTED,
} tavrn_mentorship_status_t;

typedef struct tavrn_mentorship_config {
    uint32_t offer_window_ms;
    uint32_t page_timeout_ms;
    uint32_t self_bootstrap_ms;
    uint32_t offer_suppression_ms;
    uint16_t rssi_weak_magnitude_db;
    uint16_t rssi_strong_magnitude_db;
    uint32_t rssi_weak_delay_ms;
    uint32_t rssi_strong_delay_ms;
    uint32_t jitter_min_ms;
    uint32_t jitter_max_ms;
    uint8_t page_attempts;
} tavrn_mentorship_config_t;

typedef struct tavrn_mentorship_state_snapshot {
    tavrn_mentorship_state_t state;
    tavrn_identity_width_t active_width;
    tavrn_adva_t selected_mentor;
    uint16_t selected_snapshot_id;
    uint8_t selected_mentor_present;
    uint8_t ordinary_traffic_gated;
    uint8_t full_bootstrap_admission_enabled;
    uint8_t frozen_snapshot_count;
    uint8_t active_offer_count;
    uint8_t sync_next_index;
    uint8_t sync_pages_committed;
    uint8_t pull_attempts;
    uint8_t restart_count;
    uint8_t join_originated;
} tavrn_mentorship_state_snapshot_t;

typedef struct tavrn_mentorship_counters {
    uint32_t sid8_unknown_drop;
    uint32_t sid8_collision_drop;
    uint32_t sid8_reserved_drop;
    uint32_t sync_page_duplicate;
    uint32_t sync_page_merged;
    uint32_t bootstrap_admitted_while_sid8;
    uint32_t gated_data_aodv;
    uint32_t transition_cleared;
    uint32_t join_originated;
    uint32_t join_received;
    uint32_t join_duplicate;
    uint32_t join_relayed;
} tavrn_mentorship_counters_t;

typedef struct tavrn_mentorship_event_trace {
    tavrn_codec_result_t wire_decode_result;
    tavrn_router_event_status_t router_result;
    tavrn_wire_type_t wire_type;
    tavrn_link_event_type_t link_event_type;
    uint8_t reached_wire_link_router;
} tavrn_mentorship_event_trace_t;

typedef struct tavrn_mentorship_join_key {
    tavrn_adva_t origin;
    uint16_t sequence;
    uint8_t occupied;
} tavrn_mentorship_join_key_t;

typedef enum tavrn_mentorship_pending_purpose {
    TAVRN_MENTORSHIP_PENDING_NONE = 0,
    TAVRN_MENTORSHIP_PENDING_OFFER,
    TAVRN_MENTORSHIP_PENDING_PULL,
    TAVRN_MENTORSHIP_PENDING_DATA,
    TAVRN_MENTORSHIP_PENDING_JOIN_ORIGIN,
    TAVRN_MENTORSHIP_PENDING_JOIN_RELAY,
} tavrn_mentorship_pending_purpose_t;

typedef struct tavrn_mentorship_pending_control {
    tavrn_validated_control_t control;
    tavrn_adva_t join_origin;
    uint16_t join_sequence;
    uint8_t controlled_flood;
    uint8_t purpose;
    uint8_t valid;
} tavrn_mentorship_pending_control_t;

typedef struct tavrn_mentorship {
    tavrn_router_t *router;
    tavrn_gtt_t *gtt;
    tavrn_mentorship_config_t config;
    tavrn_mentorship_state_snapshot_t state;
    tavrn_mentorship_counters_t counters;
    tavrn_mentorship_snapshot_data_t frozen_snapshot;
    tavrn_mentorship_snapshot_data_t serving_snapshot;
    tavrn_mentorship_record_t receiving_records[TAVRN_MENTORSHIP_SNAPSHOT_CAPACITY];
    tavrn_mentorship_offer_t offers[TAVRN_MENTORSHIP_OFFER_CAPACITY];
    tavrn_mentorship_offer_t pending_offer;
    tavrn_mentorship_join_key_t joins[16u];
    tavrn_mentorship_pending_control_t pending_control;
    tavrn_mentorship_pending_control_t
        join_obligations[TAVRN_MENTORSHIP_JOIN_OBLIGATION_CAPACITY];
    tavrn_adva_t serving_mentee;
    uint32_t started_at_ms;
    uint32_t page_deadline_ms;
    uint32_t offer_window_deadline_ms;
    uint32_t serving_deadline_ms;
    uint32_t offer_suppressed_until_ms;
    uint32_t pending_offer_due_ms;
    uint16_t active_page_snapshot_id;
    uint16_t serving_boot_nonce;
    uint16_t next_join_sequence;
    uint16_t receiving_page_bitmap;
    uint8_t pending_offer_valid;
    uint8_t page_session_valid;
    uint8_t serving_session_valid;
    uint8_t sync_complete_authorized;
    uint8_t identity_conflict_pending;
    uint8_t last_now_valid;
    uint32_t last_now_ms;
} tavrn_mentorship_t;

tavrn_mentorship_status_t tavrn_mentorship_init(
    tavrn_mentorship_t *mentorship, tavrn_router_t *router, tavrn_gtt_t *gtt,
    const tavrn_mentorship_config_t *config, uint32_t now_ms);

/* Retains self and every occupied, not-purged record (including hard-expired
 * members and departed tombstones), sorted by canonical AdvA byte order. */
tavrn_mentorship_status_t tavrn_mentorship_freeze_snapshot(
    tavrn_mentorship_t *mentorship, uint16_t snapshot_id, uint32_t now_ms,
    tavrn_mentorship_snapshot_data_t *snapshot_out);
tavrn_esc_context_status_t tavrn_mentorship_resolve_sid8(
    const tavrn_mentorship_t *mentorship, uint8_t sid8, uint32_t now_ms,
    tavrn_esc_context_match_t *match_out);

tavrn_mentorship_status_t tavrn_mentorship_offer_delay_ms(
    const tavrn_mentorship_config_t *config, uint8_t rssi_magnitude_db,
    uint32_t jitter_ms, uint32_t *delay_out);
tavrn_mentorship_offer_status_t tavrn_mentorship_collect_offer(
    tavrn_mentorship_t *mentorship, const tavrn_mentorship_offer_t *offer,
    uint32_t now_ms);
/* A bootstrapped mentor records one delayed offer per mentee.  A valid
 * overheard offer for that same mentee cancels the pending offer, and the
 * suppression interval blocks a duplicate re-schedule. */
tavrn_mentorship_offer_status_t tavrn_mentorship_schedule_offer(
    tavrn_mentorship_t *mentorship, const tavrn_mentorship_offer_t *offer,
    uint32_t due_ms);
tavrn_mentorship_offer_status_t tavrn_mentorship_overhear_offer(
    tavrn_mentorship_t *mentorship, const tavrn_mentorship_offer_t *offer,
    uint32_t now_ms);
tavrn_mentorship_status_t tavrn_mentorship_select_offer(
    tavrn_mentorship_t *mentorship, uint32_t now_ms,
    tavrn_mentorship_offer_t *selected_out);

/* These builders/ingesters own only full-identity bootstrap controls.  The
 * resulting controls must still pass tavrn_wire_v2 and enter router/link via
 * tavrn_mentorship_handle_scheduler_event(). */
tavrn_mentorship_status_t tavrn_mentorship_build_sync_offer(
    const tavrn_mentorship_snapshot_data_t *snapshot,
    const tavrn_adva_t *mentor, const tavrn_adva_t *mentee,
    uint16_t mentee_boot_nonce, tavrn_validated_control_t *control_out);
tavrn_mentorship_status_t tavrn_mentorship_build_sync_pull(
    const tavrn_adva_t *mentee, const tavrn_adva_t *mentor,
    uint16_t snapshot_id, uint8_t page_index,
    tavrn_validated_control_t *control_out);
tavrn_mentorship_status_t tavrn_mentorship_build_sync_data(
    const tavrn_mentorship_snapshot_data_t *snapshot,
    const tavrn_adva_t *mentor, const tavrn_adva_t *mentee, uint8_t page_index,
    tavrn_validated_control_t *control_out);
tavrn_mentorship_status_t tavrn_mentorship_ingest_sync_data(
    tavrn_mentorship_t *mentorship, const tavrn_adva_t *mentor,
    const tavrn_validated_control_t *control, uint32_t now_ms);

/* The only public integration entry for received Phase-4 traffic.  It must
 * record the real codec -> link -> router seam before applying Full bootstrap
 * policy; ordinary DATA/AODV stay gated until activation. */
tavrn_mentorship_status_t tavrn_mentorship_handle_scheduler_event(
    tavrn_mentorship_t *mentorship, const ble_mesh_sched_event_t *event,
    uint32_t now_ms, tavrn_mentorship_event_trace_t *trace_out);
tavrn_mentorship_status_t tavrn_mentorship_tick(
    tavrn_mentorship_t *mentorship, uint32_t now_ms);
tavrn_mentorship_status_t tavrn_mentorship_activate_sid8(
    tavrn_mentorship_t *mentorship, uint32_t now_ms);
tavrn_mentorship_status_t tavrn_mentorship_recover_sid16(
    tavrn_mentorship_t *mentorship, uint32_t now_ms);

/* These preserve the sole production router/AODV path after the atomic width
 * handover; callers cannot bypass the activation gate. */
aodv_status_t tavrn_mentorship_submit_application(
    tavrn_mentorship_t *mentorship, const tron_application_data_t *data,
    uint32_t now_ms);
tavrn_mentorship_status_t tavrn_mentorship_dispatch(
    tavrn_mentorship_t *mentorship, uint32_t now_ms,
    tavrn_router_phase_trace_t *trace_out);

tavrn_mentorship_status_t tavrn_mentorship_build_join(
    tavrn_mentorship_t *mentorship, uint16_t tc_sequence,
    tavrn_validated_control_t *control_out);
tavrn_mentorship_status_t tavrn_mentorship_state_snapshot(
    const tavrn_mentorship_t *mentorship,
    tavrn_mentorship_state_snapshot_t *snapshot_out);
const tavrn_mentorship_counters_t *tavrn_mentorship_counters(
    const tavrn_mentorship_t *mentorship);

#endif /* TAVRN_ESC_MENTORSHIP_API */

#endif /* TAVRN_PHASE4_ESC_MENTOR_CONTRACT_H */
