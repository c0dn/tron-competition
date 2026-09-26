#ifndef TAVRN_GTT_H
#define TAVRN_GTT_H

#include <stdint.h>

#include "tavrn_wire_v2.h"

#define TAVRN_GTT_CAPACITY 16u
#define TAVRN_GTT_HOP_MAX  15u
#define TAVRN_GTT_EXPIRY_API 1

typedef char tavrn_gtt_capacity_guard[
    (TAVRN_GTT_CAPACITY == 16u) ? 1 : -1];
typedef char tavrn_gtt_hop_guard[
    (TAVRN_GTT_HOP_MAX == 15u) ? 1 : -1];

typedef enum tavrn_gtt_freshness {
    TAVRN_GTT_FRESHNESS_ACTIVE = 0,
    TAVRN_GTT_FRESHNESS_SOFT_STALE,
    TAVRN_GTT_FRESHNESS_HARD_EXPIRED,
    TAVRN_GTT_FRESHNESS_DEPARTED,
} tavrn_gtt_freshness_t;

typedef enum tavrn_gtt_evidence_kind {
    TAVRN_GTT_EVIDENCE_LIVENESS = 0,
    TAVRN_GTT_EVIDENCE_DEPARTED,
} tavrn_gtt_evidence_kind_t;

typedef struct tavrn_gtt_evidence {
    tavrn_adva_t identity;
    uint16_t serial;
    uint8_t serial_present;
    uint8_t hop_count;
    tavrn_gtt_evidence_kind_t kind;
} tavrn_gtt_evidence_t;

typedef struct tavrn_gtt_entry {
    tavrn_adva_t identity;
    uint32_t last_evidence_ms;
    uint32_t soft_deadline_ms;
    uint32_t hard_deadline_ms;
    uint32_t departed_deadline_ms;
    uint16_t serial;
    uint8_t occupied;
    uint8_t serial_present;
    uint8_t hop_count;
    uint8_t departed;
    uint8_t direct;
    uint8_t application_requested;
    uint32_t last_direct_evidence_ms;
    uint32_t revision;
    uint32_t storage_generation;
} tavrn_gtt_entry_t;

typedef struct tavrn_gtt_storage {
    tavrn_gtt_entry_t entries[TAVRN_GTT_CAPACITY];
    uint32_t next_generation;
} tavrn_gtt_storage_t;

typedef struct tavrn_gtt_config {
    tavrn_adva_t local_identity;
    uint32_t soft_expiry_ms;
    uint32_t hard_expiry_ms;
    uint32_t departed_retention_ms;
} tavrn_gtt_config_t;

typedef struct tavrn_gtt_snapshot {
    tavrn_adva_t identity;
    uint32_t last_evidence_ms;
    uint32_t soft_deadline_ms;
    uint32_t hard_deadline_ms;
    uint32_t departed_deadline_ms;
    uint16_t serial;
    uint8_t serial_present;
    uint8_t hop_count;
    tavrn_gtt_freshness_t freshness;
} tavrn_gtt_snapshot_t;

typedef struct tavrn_gtt_counters {
    uint32_t observed;
    uint32_t self_ignored;
    uint32_t stale_evidence;
    uint32_t capacity_rejected;
    uint32_t departed;
    uint32_t resurrected;
} tavrn_gtt_counters_t;

typedef struct tavrn_gtt {
    tavrn_gtt_storage_t *storage;
    tavrn_gtt_config_t config;
    tavrn_gtt_counters_t counters;
} tavrn_gtt_t;

typedef enum tavrn_phase5_expiry_lane {
    TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER = 0,
    TAVRN_PHASE5_EXPIRY_LANE_OTHER_TASK,
} tavrn_phase5_expiry_lane_t;

typedef enum tavrn_gtt_provenance {
    TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE = 0,
    TAVRN_GTT_PROVENANCE_DIRECT_HELLO,
    TAVRN_GTT_PROVENANCE_DIRECT_OUTER_TRANSMITTER,
    TAVRN_GTT_PROVENANCE_DIRECT_BOOTSTRAP,
    TAVRN_GTT_PROVENANCE_IMPORTED_SYNC,
    TAVRN_GTT_PROVENANCE_IMPORTED_METADATA,
    TAVRN_GTT_PROVENANCE_IMPORTED_TC_SUBJECT,
    TAVRN_GTT_PROVENANCE_CONFIRMED_DEPARTURE,
    TAVRN_GTT_PROVENANCE_DIRECT_INCARNATION,
} tavrn_gtt_provenance_t;

typedef struct tavrn_gtt_expiry_snapshot {
    tavrn_adva_t identity;
    uint32_t last_evidence_ms;
    uint32_t last_direct_evidence_ms;
    uint32_t soft_deadline_ms;
    uint32_t hard_deadline_ms;
    uint32_t departed_deadline_ms;
    uint32_t revision;
    uint32_t storage_generation;
    uint16_t serial;
    uint8_t serial_present;
    uint8_t hop_count;
    uint8_t direct;
    uint8_t application_requested;
    tavrn_gtt_freshness_t freshness;
} tavrn_gtt_expiry_snapshot_t;

typedef struct tavrn_gtt_sync_record {
    tavrn_adva_t identity;
    uint32_t remaining_lifetime_ms;
    uint16_t serial;
    uint8_t serial_present;
    uint8_t hop_count;
    uint8_t departed;
} tavrn_gtt_sync_record_t;

typedef struct tavrn_gtt_departure_candidate {
    tavrn_adva_t identity;
    uint32_t revision;
    uint32_t hard_deadline_ms;
} tavrn_gtt_departure_candidate_t;

typedef enum tavrn_gtt_expiry_observe_status {
    TAVRN_GTT_EXPIRY_OBSERVE_ADDED = 0,
    TAVRN_GTT_EXPIRY_OBSERVE_REFRESHED,
    TAVRN_GTT_EXPIRY_OBSERVE_DUPLICATE,
    TAVRN_GTT_EXPIRY_OBSERVE_STALE,
    TAVRN_GTT_EXPIRY_OBSERVE_SELF_IGNORED,
    TAVRN_GTT_EXPIRY_OBSERVE_REPLACED_DEPARTED,
    TAVRN_GTT_EXPIRY_OBSERVE_REPLACED_OLDEST_STALE,
    TAVRN_GTT_EXPIRY_OBSERVE_NO_VICTIM,
    TAVRN_GTT_EXPIRY_OBSERVE_INVALID,
    TAVRN_GTT_EXPIRY_OBSERVE_UNAVAILABLE,
} tavrn_gtt_expiry_observe_status_t;

typedef enum tavrn_gtt_expiry_query_status {
    TAVRN_GTT_EXPIRY_QUERY_FOUND = 0,
    TAVRN_GTT_EXPIRY_QUERY_NOT_FOUND,
    TAVRN_GTT_EXPIRY_QUERY_OUTPUT_TOO_SMALL,
    TAVRN_GTT_EXPIRY_QUERY_INVALID,
    TAVRN_GTT_EXPIRY_QUERY_MARSHAL_REQUIRED,
    TAVRN_GTT_EXPIRY_QUERY_UNAVAILABLE,
} tavrn_gtt_expiry_query_status_t;

typedef enum tavrn_gtt_application_request_status {
    TAVRN_GTT_APPLICATION_CHANGED = 0,
    TAVRN_GTT_APPLICATION_UNCHANGED_DUPLICATE,
    TAVRN_GTT_APPLICATION_CANCELED,
    TAVRN_GTT_APPLICATION_SELF,
    TAVRN_GTT_APPLICATION_NOT_FOUND,
    TAVRN_GTT_APPLICATION_DEPARTED,
    TAVRN_GTT_APPLICATION_MARSHAL_REQUIRED,
    TAVRN_GTT_APPLICATION_INVALID,
    TAVRN_GTT_APPLICATION_UNAVAILABLE,
} tavrn_gtt_application_request_status_t;

/* Mesh-owner-only force-expiry seam used by the bounded failed-hop liveness
 * policy.  It never creates a member or fabricates evidence. */
typedef enum tavrn_gtt_force_due_status {
    TAVRN_GTT_FORCE_DUE_CHANGED = 0,
    TAVRN_GTT_FORCE_DUE_ALREADY_DUE,
    TAVRN_GTT_FORCE_DUE_NOT_FOUND,
    TAVRN_GTT_FORCE_DUE_DEPARTED,
    TAVRN_GTT_FORCE_DUE_SELF,
    TAVRN_GTT_FORCE_DUE_MARSHAL_REQUIRED,
    TAVRN_GTT_FORCE_DUE_INVALID,
    TAVRN_GTT_FORCE_DUE_UNAVAILABLE,
} tavrn_gtt_force_due_status_t;

typedef enum tavrn_gtt_application_command_kind {
    TAVRN_GTT_APPLICATION_COMMAND_REQUEST = 0,
    TAVRN_GTT_APPLICATION_COMMAND_CANCEL,
    TAVRN_GTT_APPLICATION_COMMAND_QUERY,
} tavrn_gtt_application_command_kind_t;

typedef struct tavrn_gtt_application_command {
    tavrn_adva_t identity;
    uint32_t now_ms;
    uint32_t query_time_ms;
    tavrn_gtt_application_command_kind_t kind;
} tavrn_gtt_application_command_t;

typedef struct tavrn_gtt_application_command_result {
    tavrn_gtt_application_request_status_t status;
    tavrn_gtt_expiry_query_status_t query_status;
    tavrn_gtt_expiry_snapshot_t snapshot;
} tavrn_gtt_application_command_result_t;

typedef enum tavrn_gtt_sync_merge_status {
    TAVRN_GTT_SYNC_MERGE_COMMITTED = 0,
    TAVRN_GTT_SYNC_MERGE_UNCHANGED,
    TAVRN_GTT_SYNC_MERGE_INVALID,
    TAVRN_GTT_SYNC_MERGE_UNAVAILABLE,
} tavrn_gtt_sync_merge_status_t;

/* General metadata carries a bounded remaining-lifetime claim, never a GTT
 * serial.  The merge therefore preserves any subject serial/direct evidence
 * already retained by the table and refuses to revive a departed tombstone. */
typedef enum tavrn_gtt_metadata_merge_status {
    TAVRN_GTT_METADATA_MERGE_COMMITTED = 0,
    TAVRN_GTT_METADATA_MERGE_UNCHANGED,
    TAVRN_GTT_METADATA_MERGE_TOMBSTONE,
    TAVRN_GTT_METADATA_MERGE_INVALID,
    TAVRN_GTT_METADATA_MERGE_UNAVAILABLE,
} tavrn_gtt_metadata_merge_status_t;

typedef enum tavrn_gtt_init_status {
    TAVRN_GTT_INIT_OK = 0,
    TAVRN_GTT_INIT_INVALID_ARGUMENT,
    TAVRN_GTT_INIT_INVALID_CONFIG,
} tavrn_gtt_init_status_t;

typedef enum tavrn_gtt_observe_status {
    TAVRN_GTT_OBSERVE_ADDED = 0,
    TAVRN_GTT_OBSERVE_REFRESHED,
    TAVRN_GTT_OBSERVE_DEPARTED,
    TAVRN_GTT_OBSERVE_STALE,
    TAVRN_GTT_OBSERVE_SELF_IGNORED,
    TAVRN_GTT_OBSERVE_CAPACITY_REJECTED,
    TAVRN_GTT_OBSERVE_INVALID,
} tavrn_gtt_observe_status_t;

typedef enum tavrn_gtt_query_status {
    TAVRN_GTT_QUERY_FOUND = 0,
    TAVRN_GTT_QUERY_NOT_FOUND,
    TAVRN_GTT_QUERY_OUTPUT_TOO_SMALL,
    TAVRN_GTT_QUERY_INVALID,
} tavrn_gtt_query_status_t;

/* Clearing a serial deliberately retains every other membership fact.  It is
 * used when a direct peer has committed a new boot incarnation, so the first
 * ordinary HELLO of that incarnation is not compared with the prior one. */
typedef enum tavrn_gtt_serial_clear_status {
    TAVRN_GTT_SERIAL_CLEAR_FOUND = 0,
    TAVRN_GTT_SERIAL_CLEAR_NOT_FOUND,
    TAVRN_GTT_SERIAL_CLEAR_INVALID,
} tavrn_gtt_serial_clear_status_t;

/* Returns true only for 0 < candidate-current < 0x8000. */
int tavrn_gtt_serial_is_newer(uint16_t candidate, uint16_t current);

tavrn_gtt_init_status_t tavrn_gtt_init(tavrn_gtt_t *gtt,
                                        tavrn_gtt_storage_t *storage,
                                        const tavrn_gtt_config_t *config,
                                        uint32_t now_ms);
tavrn_gtt_observe_status_t tavrn_gtt_observe(
    tavrn_gtt_t *gtt, const tavrn_gtt_evidence_t *evidence, uint32_t now_ms);
tavrn_gtt_query_status_t tavrn_gtt_snapshot(
    const tavrn_gtt_t *gtt, const tavrn_adva_t *identity, uint32_t now_ms,
    tavrn_gtt_snapshot_t *snapshot_out);
tavrn_gtt_serial_clear_status_t tavrn_gtt_clear_serial(
    tavrn_gtt_t *gtt, const tavrn_adva_t *identity, uint32_t now_ms);
tavrn_gtt_query_status_t tavrn_gtt_enumerate_active(
    const tavrn_gtt_t *gtt, uint32_t now_ms,
    tavrn_gtt_snapshot_t *snapshots_out, uint8_t snapshot_capacity,
    uint8_t *snapshot_count_out);
/* Counts occupied, non-local, non-departed records.  Retained soft- and
 * hard-expired liveness records remain known until table eviction. */
uint8_t tavrn_gtt_known_remote_count(const tavrn_gtt_t *gtt);
const tavrn_gtt_counters_t *tavrn_gtt_counters(const tavrn_gtt_t *gtt);

tavrn_gtt_expiry_observe_status_t tavrn_gtt_observe_with_provenance(
    tavrn_gtt_t *gtt, const tavrn_gtt_evidence_t *evidence,
    tavrn_gtt_provenance_t provenance, uint32_t now_ms,
    tavrn_gtt_expiry_snapshot_t *snapshot_out);
tavrn_gtt_expiry_query_status_t tavrn_gtt_expiry_snapshot(
    const tavrn_gtt_t *gtt, const tavrn_adva_t *identity, uint32_t now_ms,
    tavrn_gtt_expiry_snapshot_t *snapshot_out);
tavrn_gtt_expiry_query_status_t tavrn_gtt_enumerate_known(
    const tavrn_gtt_t *gtt, tavrn_phase5_expiry_lane_t lane, uint32_t now_ms,
    tavrn_gtt_expiry_snapshot_t *snapshots_out, uint8_t capacity,
    uint8_t *count_out);
tavrn_gtt_application_request_status_t tavrn_gtt_application_request(
    tavrn_gtt_t *gtt, tavrn_phase5_expiry_lane_t lane,
    const tavrn_adva_t *identity, uint32_t now_ms,
    tavrn_gtt_expiry_snapshot_t *snapshot_out);
tavrn_gtt_application_request_status_t tavrn_gtt_application_cancel(
    tavrn_gtt_t *gtt, tavrn_phase5_expiry_lane_t lane,
    const tavrn_adva_t *identity, uint32_t now_ms,
    tavrn_gtt_expiry_snapshot_t *snapshot_out);
tavrn_gtt_application_command_result_t tavrn_gtt_apply_application_command(
    tavrn_gtt_t *gtt, tavrn_phase5_expiry_lane_t lane,
    tavrn_gtt_application_command_t command);
tavrn_gtt_force_due_status_t tavrn_gtt_force_due(
    tavrn_gtt_t *gtt, tavrn_phase5_expiry_lane_t lane,
    const tavrn_adva_t *identity, uint32_t now_ms,
    tavrn_gtt_expiry_snapshot_t *snapshot_out);
tavrn_gtt_sync_merge_status_t tavrn_gtt_sync_merge(
    tavrn_gtt_t *gtt, const tavrn_gtt_sync_record_t *records,
    uint8_t record_count, uint32_t now_ms);
tavrn_gtt_metadata_merge_status_t tavrn_gtt_metadata_merge(
    tavrn_gtt_t *gtt, const tavrn_adva_t *identity, uint8_t ttl_bucket,
    uint32_t now_ms);

#endif /* TAVRN_GTT_H */
