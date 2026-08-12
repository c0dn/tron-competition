#include "tavrn_maintenance.h"

#include <string.h>

#include "tron_timer_config.h"

#define TAVRN_MAINTENANCE_HALF_RANGE 0x80000000u
#define TAVRN_MAINTENANCE_PERMILLE 1000u
#define TAVRN_MAINTENANCE_LIVENESS_FACTOR 3u
#define TAVRN_MAINTENANCE_LIVENESS_SNAP_PERMILLE 1050u

static int time_due(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static int sequence_after(uint16_t candidate, uint16_t current)
{
    return (int16_t)(candidate - current) > 0;
}

static void shared_sequence_frontier(tavrn_maintenance_t *maintenance)
{
    if (maintenance->targeted_frontier_valid == 0u) {
        maintenance->targeted_reservation_frontier =
            maintenance->snapshot.next_node_sequence;
        maintenance->targeted_frontier_valid = 1u;
    }
}

static uint16_t shared_sequence_reserve_targeted(tavrn_maintenance_t *maintenance)
{
    uint16_t sequence;

    shared_sequence_frontier(maintenance);
    /* The ordinary control has already claimed the visible sequence while it
     * waits for scheduler admission.  Targeted work starts strictly after it. */
    if (maintenance->snapshot.pending != 0u &&
        maintenance->targeted_reservation_frontier ==
            ((uint16_t)maintenance->snapshot.pending_hello.pdu[15] |
             ((uint16_t)maintenance->snapshot.pending_hello.pdu[16] << 8))) {
        maintenance->targeted_reservation_frontier++;
    }
    sequence = maintenance->targeted_reservation_frontier;
    maintenance->targeted_reservation_frontier++;
    /* A retained ordinary HELLO still owns the public cursor.  Targeted
     * reservations stay private until that ordinary sequence is actually
     * admitted, at which point shared_sequence_commit_ordinary() merges this
     * frontier atomically with the ordinary cursor. */
    if (maintenance->snapshot.pending == 0u) {
        maintenance->snapshot.next_node_sequence =
            maintenance->targeted_reservation_frontier;
    }
    return sequence;
}

static void shared_sequence_commit_ordinary(tavrn_maintenance_t *maintenance,
                                            uint16_t admitted_sequence)
{
    uint16_t ordinary_next = (uint16_t)(admitted_sequence + 1u);

    shared_sequence_frontier(maintenance);
    if (maintenance->targeted_reservation_frontier != ordinary_next &&
        !sequence_after(maintenance->targeted_reservation_frontier, ordinary_next)) {
        maintenance->targeted_reservation_frontier = ordinary_next;
    }
    maintenance->snapshot.next_node_sequence =
        maintenance->targeted_reservation_frontier;
}

static int adva_equal(const tavrn_adva_t *left, const tavrn_adva_t *right)
{
    return left != NULL && right != NULL &&
        memcmp(left->bytes, right->bytes, TAVRN_ADVA_LEN) == 0;
}

static int config_is_valid(const tavrn_maintenance_config_t *config)
{
    uint8_t adaptive_all_zero;

    if (config == NULL || config->hello_change_ms == 0u ||
        config->hello_change_ms >= TAVRN_MAINTENANCE_HALF_RANGE ||
        config->hello_dedupe_ms == 0u ||
        config->hello_dedupe_ms >= TAVRN_MAINTENANCE_HALF_RANGE) {
        return 0;
    }
    adaptive_all_zero = config->hello_stable_ms == 0u &&
        config->hello_alpha_permille == 0u &&
        config->hello_snap_permille == 0u &&
        config->topology_sample_ms == 0u;
    if (adaptive_all_zero != 0u) {
        return 1;
    }
    return config->hello_stable_ms >= config->hello_change_ms &&
        config->hello_stable_ms < TAVRN_MAINTENANCE_HALF_RANGE &&
        config->hello_alpha_permille != 0u &&
        config->hello_alpha_permille <= TAVRN_MAINTENANCE_PERMILLE &&
        config->hello_snap_permille != 0u &&
        config->hello_snap_permille <= TAVRN_MAINTENANCE_PERMILLE &&
        config->topology_sample_ms != 0u &&
        config->topology_sample_ms < TAVRN_MAINTENANCE_HALF_RANGE;
}

static int adaptive_enabled(const tavrn_maintenance_t *maintenance)
{
    return maintenance != NULL && maintenance->config.hello_stable_ms != 0u;
}

static uint32_t saturating_multiply(uint32_t value, uint32_t factor)
{
    if (factor != 0u && value > UINT32_MAX / factor) {
        return UINT32_MAX;
    }
    return value * factor;
}

static uint32_t liveness_threshold(uint32_t interval_ms)
{
    return saturating_multiply(interval_ms, TAVRN_MAINTENANCE_LIVENESS_FACTOR);
}

static uint32_t liveness_snap_threshold(uint32_t threshold_ms)
{
    uint64_t scaled = (uint64_t)threshold_ms *
        TAVRN_MAINTENANCE_LIVENESS_SNAP_PERMILLE;

    return scaled >= (uint64_t)UINT32_MAX * TAVRN_MAINTENANCE_PERMILLE ?
        UINT32_MAX : (uint32_t)(scaled / TAVRN_MAINTENANCE_PERMILLE);
}

static void update_liveness_timeout(tavrn_maintenance_t *maintenance,
                                    uint8_t decay_floor)
{
    uint32_t current_threshold;
    uint32_t floor;

    if (maintenance == NULL || adaptive_enabled(maintenance) == 0) {
        return;
    }
    current_threshold = liveness_threshold(maintenance->snapshot.current_interval_ms);
    floor = maintenance->snapshot.liveness_floor_ms;
    maintenance->snapshot.liveness_timeout_ms =
        floor > current_threshold ? floor : current_threshold;
    if (decay_floor == 0u || floor <= current_threshold) {
        return;
    }
    floor -= (floor - current_threshold) / 2u;
    if (floor <= liveness_snap_threshold(current_threshold)) {
        floor = 0u;
    }
    maintenance->snapshot.liveness_floor_ms = floor;
    maintenance->counters.liveness_floor_decayed++;
}

static uint8_t direct_one_hop_count(const tavrn_maintenance_t *maintenance,
                                    uint32_t now_ms)
{
    uint8_t index;
    uint8_t count = 0u;

    (void)now_ms;
    if (maintenance == NULL || maintenance->gtt == NULL ||
        maintenance->gtt->storage == NULL) {
        return 0u;
    }
    for (index = 0u; index < TAVRN_GTT_CAPACITY; index++) {
        const tavrn_gtt_entry_t *entry = &maintenance->gtt->storage->entries[index];

        if (entry->occupied == 0u || entry->departed != 0u ||
            entry->hop_count != 1u ||
            adva_equal(&entry->identity, &maintenance->gtt->config.local_identity)) {
            continue;
        }
        count++;
    }
    return count;
}

static uint32_t adaptive_next_interval(const tavrn_maintenance_t *maintenance,
                                       uint8_t *snapped_out)
{
    uint64_t numerator;
    uint32_t interval;
    uint32_t snap_at;

    *snapped_out = 0u;
    numerator = (uint64_t)maintenance->config.hello_alpha_permille *
        maintenance->snapshot.current_interval_ms +
        (uint64_t)(TAVRN_MAINTENANCE_PERMILLE -
                   maintenance->config.hello_alpha_permille) *
            maintenance->config.hello_stable_ms +
        TAVRN_MAINTENANCE_PERMILLE / 2u;
    interval = (uint32_t)(numerator / TAVRN_MAINTENANCE_PERMILLE);
    if (interval > maintenance->config.hello_stable_ms) {
        interval = maintenance->config.hello_stable_ms;
    }
    if (interval <= maintenance->snapshot.current_interval_ms &&
        maintenance->snapshot.current_interval_ms <
            maintenance->config.hello_stable_ms) {
        interval = maintenance->snapshot.current_interval_ms + 1u;
    }
    snap_at = (uint32_t)(((uint64_t)maintenance->config.hello_snap_permille *
                           maintenance->config.hello_stable_ms) /
                          TAVRN_MAINTENANCE_PERMILLE);
    if (interval >= snap_at && interval != maintenance->config.hello_stable_ms) {
        interval = maintenance->config.hello_stable_ms;
        *snapped_out = 1u;
    }
    return interval;
}

static void advance_adaptive_interval(tavrn_maintenance_t *maintenance)
{
    uint8_t snapped;

    if (adaptive_enabled(maintenance) == 0) {
        return;
    }
    maintenance->snapshot.current_interval_ms =
        adaptive_next_interval(maintenance, &snapped);
    maintenance->counters.interval_advanced++;
    if (snapped != 0u) {
        maintenance->counters.interval_snapped++;
    }
    update_liveness_timeout(maintenance, 0u);
}

/* Sampling uses only retained active direct GTT evidence.  It runs before a
 * due HELLO so a topology change cannot emit one stale-cadence HELLO first. */
static uint8_t sample_topology(tavrn_maintenance_t *maintenance, uint32_t now_ms)
{
    uint8_t direct_count;

    direct_count = direct_one_hop_count(maintenance, now_ms);
    maintenance->counters.liveness_sample++;
    maintenance->snapshot.next_topology_sample_ms = now_ms +
        maintenance->config.topology_sample_ms;
    if (direct_count != maintenance->snapshot.direct_one_hop_count) {
        uint32_t old_threshold =
            liveness_threshold(maintenance->snapshot.current_interval_ms);

        if (old_threshold > maintenance->snapshot.liveness_floor_ms) {
            maintenance->snapshot.liveness_floor_ms = old_threshold;
        }
        maintenance->snapshot.direct_one_hop_count = direct_count;
        maintenance->snapshot.current_interval_ms =
            maintenance->config.hello_change_ms;
        maintenance->snapshot.next_hello_due_ms = now_ms +
            maintenance->snapshot.current_interval_ms;
        maintenance->counters.topology_reset++;
        /* Retain the pre-change liveness floor through this newly observed
         * membership epoch. The next unchanged sample performs its bounded
         * decay; expiry gating therefore cannot shorten a direct lifetime at
         * the same transition that reset the HELLO cadence. */
        update_liveness_timeout(maintenance, 0u);
        return 1u;
    }
    maintenance->counters.topology_unchanged++;
    update_liveness_timeout(maintenance, 1u);
    return 0u;
}

static int maintenance_is_initialized(const tavrn_maintenance_t *maintenance)
{
    return maintenance != NULL && maintenance->router != NULL &&
        maintenance->gtt != NULL && maintenance->gtt->storage != NULL &&
        config_is_valid(&maintenance->config);
}

static int sid8_active(const tavrn_maintenance_t *maintenance)
{
    const tavrn_router_t *router;

    if (!maintenance_is_initialized(maintenance)) {
        return 0;
    }
    router = maintenance->router;
    return router->link != NULL && router->incarnation.enabled != 0u &&
        router->incarnation.config.feature_level ==
            TAVRN_ROUTER_FEATURE_FULL_TAVRN &&
        router->incarnation.snapshot.state ==
            TAVRN_ROUTER_INCARNATION_ESTABLISHED &&
        router->link->config.local_peer.logical_id.width == TAVRN_IDENTITY_SID8;
}

static void clear_pending(tavrn_maintenance_t *maintenance)
{
    if (maintenance == NULL) {
        return;
    }
    memset(&maintenance->snapshot.pending_hello, 0,
           sizeof(maintenance->snapshot.pending_hello));
    maintenance->snapshot.pending = 0u;
    maintenance->counters.pending = 0u;
}

static void disarm(tavrn_maintenance_t *maintenance)
{
    if (maintenance == NULL) {
        return;
    }
    if (maintenance->queued_hello_valid != 0u) {
        (void)tavrn_router_cancel_ordinary_hello(maintenance->router,
                                                  &maintenance->queued_hello);
    }
    clear_pending(maintenance);
    memset(&maintenance->queued_hello, 0, sizeof(maintenance->queued_hello));
    maintenance->queued_hello_valid = 0u;
    maintenance->local_broadcast_suppression_active = 0u;
    maintenance->snapshot.armed = 0u;
    maintenance->snapshot.next_hello_due_ms = 0u;
    maintenance->snapshot.next_topology_sample_ms = 0u;
    maintenance->snapshot.current_interval_ms = 0u;
    maintenance->snapshot.liveness_floor_ms = 0u;
    maintenance->snapshot.liveness_timeout_ms = 0u;
    maintenance->snapshot.direct_one_hop_count = 0u;
    memset(maintenance->dedupe, 0, sizeof(maintenance->dedupe));
}

static void purge_dedupe(tavrn_maintenance_t *maintenance, uint32_t now_ms)
{
    uint8_t index;

    for (index = 0u; index < TAVRN_MAINTENANCE_DEDUPE_CAPACITY; index++) {
        tavrn_maintenance_dedupe_entry_t *entry = &maintenance->dedupe[index];

        if (entry->valid != 0u && time_due(now_ms, entry->expires_at_ms)) {
            memset(entry, 0, sizeof(*entry));
        }
    }
}

static void clear_dedupe_for_origin(tavrn_maintenance_t *maintenance,
                                    const tavrn_adva_t *origin)
{
    uint8_t index;

    for (index = 0u; index < TAVRN_MAINTENANCE_DEDUPE_CAPACITY; index++) {
        tavrn_maintenance_dedupe_entry_t *entry = &maintenance->dedupe[index];

        if (entry->valid != 0u && adva_equal(&entry->origin, origin)) {
            memset(entry, 0, sizeof(*entry));
        }
    }
}

static tavrn_maintenance_epoch_entry_t *epoch_slot(
    tavrn_maintenance_t *maintenance, const tavrn_adva_t *origin)
{
    uint8_t index;

    for (index = 0u; index < TAVRN_MAINTENANCE_EPOCH_CAPACITY; index++) {
        tavrn_maintenance_epoch_entry_t *entry = &maintenance->epochs[index];

        if (entry->valid != 0u && adva_equal(&entry->origin, origin)) {
            return entry;
        }
    }
    for (index = 0u; index < TAVRN_MAINTENANCE_EPOCH_CAPACITY; index++) {
        if (maintenance->epochs[index].valid == 0u) {
            return &maintenance->epochs[index];
        }
    }
    return NULL;
}

/* A reset that is still pending keeps the old tuple visible in router-common.
 * Do not advance this local epoch until the shared reset has committed. */
static tavrn_maintenance_status_t synchronize_peer_epoch(
    tavrn_maintenance_t *maintenance, const tavrn_adva_t *origin,
    uint32_t now_ms)
{
    tavrn_router_incarnation_peer_snapshot_t peer;
    tavrn_router_incarnation_status_t peer_status;
    tavrn_maintenance_epoch_entry_t *entry;
    tavrn_gtt_serial_clear_status_t clear_status;

    peer_status = tavrn_router_incarnation_peer_snapshot(
        maintenance->router, origin, &peer);
    if (peer_status == TAVRN_ROUTER_INCARNATION_NOT_FOUND) {
        return TAVRN_MAINTENANCE_OK;
    }
    if (peer_status != TAVRN_ROUTER_INCARNATION_OK) {
        return TAVRN_MAINTENANCE_INVALID;
    }
    if (peer.direct_binding_valid == 0u || peer.route_barred != 0u ||
        peer.reset_pending != 0u) {
        return TAVRN_MAINTENANCE_OK;
    }
    entry = epoch_slot(maintenance, origin);
    if (entry == NULL) {
        return TAVRN_MAINTENANCE_INVALID;
    }
    if (entry->valid != 0u && entry->boot_nonce == peer.boot_nonce) {
        return TAVRN_MAINTENANCE_OK;
    }
    clear_status = tavrn_gtt_clear_serial(maintenance->gtt, origin, now_ms);
    if (clear_status == TAVRN_GTT_SERIAL_CLEAR_INVALID) {
        return TAVRN_MAINTENANCE_INVALID;
    }
    clear_dedupe_for_origin(maintenance, origin);
    memset(entry, 0, sizeof(*entry));
    entry->origin = *origin;
    entry->boot_nonce = peer.boot_nonce;
    entry->valid = 1u;
    return TAVRN_MAINTENANCE_OK;
}

static tavrn_maintenance_dedupe_entry_t *dedupe_slot(
    tavrn_maintenance_t *maintenance, const tavrn_adva_t *origin,
    uint16_t node_sequence, uint32_t now_ms, uint8_t *duplicate_out)
{
    uint8_t index;

    *duplicate_out = 0u;
    purge_dedupe(maintenance, now_ms);
    for (index = 0u; index < TAVRN_MAINTENANCE_DEDUPE_CAPACITY; index++) {
        tavrn_maintenance_dedupe_entry_t *entry = &maintenance->dedupe[index];

        if (entry->valid != 0u && entry->targeted == 0u &&
            entry->node_sequence == node_sequence &&
            adva_equal(&entry->origin, origin)) {
            *duplicate_out = 1u;
            return entry;
        }
    }
    for (index = 0u; index < TAVRN_MAINTENANCE_DEDUPE_CAPACITY; index++) {
        if (maintenance->dedupe[index].valid == 0u) {
            return &maintenance->dedupe[index];
        }
    }
    return NULL;
}

static void remember_dedupe(tavrn_maintenance_dedupe_entry_t *entry,
                            const tavrn_adva_t *origin,
                            uint16_t node_sequence,
                            const tavrn_maintenance_config_t *config,
                            uint32_t now_ms)
{
    memset(entry, 0, sizeof(*entry));
    entry->origin = *origin;
    entry->node_sequence = node_sequence;
    entry->expires_at_ms = now_ms + config->hello_dedupe_ms;
    entry->valid = 1u;
}

static int ordinary_hello_event(const tavrn_maintenance_t *maintenance,
                                 const tavrn_rx_control_event_t *event,
                                 tavrn_adva_t *origin_out,
                                 uint16_t *node_sequence_out,
                                 uint8_t *known_remote_count_out)
{
    const tavrn_validated_control_t *control;
    const tavrn_direct_peer_t *transmitter;
    tavrn_adva_t origin;

    if (maintenance == NULL || event == NULL || origin_out == NULL ||
        node_sequence_out == NULL || known_remote_count_out == NULL) {
        return 0;
    }
    control = &event->control;
    transmitter = &event->transmitter;
    if (control->type != TAVRN_WIRE_HELLO || control->pdu_len != 20u ||
        control->pdu[0] != 0x54u || control->pdu[1] != 0x52u ||
        control->pdu[2] != 0x02u ||
        control->pdu[3] != maintenance->router->link->config.network_id ||
        control->pdu[4] != TAVRN_WIRE_HELLO || control->pdu[5] != 0x80u ||
        control->pdu[6] != 0x10u || control->pdu[7] != 0xffu ||
        control->pdu[8] != 0xffu || control->pdu[17] > 15u ||
        control->pdu[18] != 0u || control->pdu[19] != 0u ||
        transmitter->logical_id.width != TAVRN_IDENTITY_SID8) {
        return 0;
    }
    memcpy(origin.bytes, &control->pdu[9], TAVRN_ADVA_LEN);
    if (!adva_equal(&origin, &transmitter->adva) ||
        transmitter->logical_id.value != origin.bytes[0] ||
        adva_equal(&origin, &maintenance->gtt->config.local_identity)) {
        return 0;
    }
    *origin_out = origin;
    *node_sequence_out = (uint16_t)control->pdu[15] |
        ((uint16_t)control->pdu[16] << 8);
    *known_remote_count_out = control->pdu[17];
    return 1;
}

static int ordinary_n0_sid8_hello(const tavrn_rx_control_event_t *event)
{
    return event != NULL && event->control.type == TAVRN_WIRE_HELLO &&
        event->control.pdu_len >= 6u &&
        (event->control.pdu[5] & 0xc0u) == 0x80u;
}

static tavrn_maintenance_status_t enqueue_pending(tavrn_maintenance_t *maintenance,
                                                   uint32_t now_ms)
{
    tavrn_router_hello_status_t status;

    status = tavrn_router_enqueue_ordinary_hello(
        maintenance->router, &maintenance->snapshot.pending_hello, now_ms);
    if (status == TAVRN_ROUTER_HELLO_BUSY) {
        maintenance->counters.busy++;
        return TAVRN_MAINTENANCE_BUSY;
    }
    if (status != TAVRN_ROUTER_HELLO_OK) {
        clear_pending(maintenance);
        return TAVRN_MAINTENANCE_INVALID;
    }
    maintenance->queued_hello = maintenance->snapshot.pending_hello;
    maintenance->queued_hello_valid = 1u;
    clear_pending(maintenance);
    maintenance->counters.enqueued++;
    shared_sequence_commit_ordinary(
        maintenance, (uint16_t)maintenance->queued_hello.pdu[15] |
        ((uint16_t)maintenance->queued_hello.pdu[16] << 8));
    advance_adaptive_interval(maintenance);
    maintenance->snapshot.next_hello_due_ms = now_ms +
        maintenance->snapshot.current_interval_ms;
    maintenance->local_broadcast_suppression_active = 0u;
    return TAVRN_MAINTENANCE_OK;
}

tavrn_maintenance_status_t tavrn_maintenance_init(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router, tavrn_gtt_t *gtt,
    const tavrn_maintenance_config_t *config)
{
    if (maintenance == NULL || router == NULL || gtt == NULL || gtt->storage == NULL ||
        !config_is_valid(config)) {
        return TAVRN_MAINTENANCE_INVALID;
    }
    memset(maintenance, 0, sizeof(*maintenance));
    maintenance->router = router;
    maintenance->gtt = gtt;
    maintenance->config = *config;
#if defined(BLE_RADIO_HOST_TEST)
    if (maintenance->config.aodv_net_traversal_ms == 0u) {
        maintenance->config.aodv_net_traversal_ms =
            tron_timer_config.aodv_net_traversal_ms;
    }
    if (maintenance->config.freshness_response_min_ms == 0u) {
        maintenance->config.freshness_response_min_ms =
            tron_timer_config.freshness_response_min_ms;
    }
    if (maintenance->config.freshness_response_max_ms == 0u) {
        maintenance->config.freshness_response_max_ms =
            tron_timer_config.freshness_response_max_ms;
    }
    if (maintenance->config.metadata_cooldown_ms == 0u) {
        maintenance->config.metadata_cooldown_ms = tron_timer_config.metadata_cooldown_ms;
    }
    if (maintenance->config.tc_uuid_ms == 0u) {
        maintenance->config.tc_uuid_ms = tron_timer_config.tc_uuid_ms;
    }
    if (maintenance->config.tc_subject_ms == 0u) {
        maintenance->config.tc_subject_ms = tron_timer_config.tc_subject_ms;
    }
#endif
    if (maintenance->config.aodv_net_traversal_ms == 0u ||
        maintenance->config.aodv_net_traversal_ms >= TAVRN_MAINTENANCE_HALF_RANGE ||
        maintenance->config.freshness_response_min_ms == 0u ||
         maintenance->config.freshness_response_max_ms <
             maintenance->config.freshness_response_min_ms ||
         maintenance->config.freshness_response_max_ms >=
             TAVRN_MAINTENANCE_HALF_RANGE ||
         maintenance->config.metadata_cooldown_ms == 0u ||
         maintenance->config.metadata_cooldown_ms >= TAVRN_MAINTENANCE_HALF_RANGE ||
         maintenance->config.tc_uuid_ms == 0u ||
         maintenance->config.tc_uuid_ms >= TAVRN_MAINTENANCE_HALF_RANGE ||
         maintenance->config.tc_subject_ms == 0u ||
         maintenance->config.tc_subject_ms >= TAVRN_MAINTENANCE_HALF_RANGE) {
        memset(maintenance, 0, sizeof(*maintenance));
        return TAVRN_MAINTENANCE_INVALID;
    }
    maintenance->snapshot.current_interval_ms = config->hello_change_ms;
    maintenance->snapshot.next_node_sequence = config->initial_node_sequence;
    maintenance->targeted_next_token = 0x7fffu;
    maintenance->targeted_reservation_frontier = config->initial_node_sequence;
    maintenance->expiry_schedule_initialized = 0u;
    {
        tavrn_tc_metadata_config_t tc_config;

        memset(&tc_config, 0, sizeof(tc_config));
        tc_config.local_identity = gtt->config.local_identity;
        tc_config.initial_tc_sequence = config->initial_node_sequence == 0u ? 1u :
            config->initial_node_sequence;
        tc_config.tc_uuid_ms = maintenance->config.tc_uuid_ms;
        tc_config.tc_subject_ms = maintenance->config.tc_subject_ms;
        tc_config.metadata_cooldown_ms = maintenance->config.metadata_cooldown_ms;
        tc_config.network_id = router->link->config.network_id;
        if (tavrn_maintenance_tc_metadata_init(&maintenance->tc_metadata,
                                               &tc_config) != TAVRN_TC_METADATA_OK ||
            tavrn_maintenance_tc_metadata_bind_gtt(&maintenance->tc_metadata, gtt) !=
                TAVRN_TC_METADATA_OK) {
            memset(maintenance, 0, sizeof(*maintenance));
            return TAVRN_MAINTENANCE_INVALID;
        }
    }
    return TAVRN_MAINTENANCE_OK;
}

tavrn_maintenance_status_t tavrn_maintenance_set_local_tc_retained_port(
    tavrn_maintenance_t *maintenance,
    const tavrn_maintenance_local_tc_retained_port_t *port_or_null)
{
    if (!maintenance_is_initialized(maintenance)) {
        return TAVRN_MAINTENANCE_INVALID;
    }
    memset(&maintenance->local_tc_retained, 0,
           sizeof(maintenance->local_tc_retained));
    if (port_or_null != NULL) {
        maintenance->local_tc_retained = *port_or_null;
    }
    return TAVRN_MAINTENANCE_OK;
}

tavrn_maintenance_status_t tavrn_maintenance_activate(
    tavrn_maintenance_t *maintenance, uint32_t now_ms)
{
    if (!maintenance_is_initialized(maintenance)) {
        return TAVRN_MAINTENANCE_INVALID;
    }
    if (!sid8_active(maintenance)) {
        disarm(maintenance);
        return TAVRN_MAINTENANCE_GATED;
    }
    if (maintenance->snapshot.armed != 0u) {
        return TAVRN_MAINTENANCE_OK;
    }
    maintenance->snapshot.armed = 1u;
    maintenance->snapshot.current_interval_ms = maintenance->config.hello_change_ms;
    maintenance->snapshot.next_hello_due_ms = now_ms +
        maintenance->snapshot.current_interval_ms;
    maintenance->expiry_next_due_ms = 0u;
    maintenance->expiry_cursor = 0u;
    maintenance->expiry_schedule_initialized = 0u;
    if (adaptive_enabled(maintenance) != 0) {
        maintenance->snapshot.direct_one_hop_count =
            direct_one_hop_count(maintenance, now_ms);
        maintenance->snapshot.next_topology_sample_ms = now_ms +
            maintenance->config.topology_sample_ms;
        maintenance->snapshot.liveness_floor_ms = 0u;
        update_liveness_timeout(maintenance, 0u);
    }
    return TAVRN_MAINTENANCE_OK;
}

tavrn_maintenance_status_t tavrn_maintenance_tick(
    tavrn_maintenance_t *maintenance, uint32_t now_ms)
{
    tavrn_router_hello_status_t build_status;
    uint8_t topology_changed = 0u;

    if (!maintenance_is_initialized(maintenance)) {
        return TAVRN_MAINTENANCE_INVALID;
    }
    if (!sid8_active(maintenance) || maintenance->snapshot.armed == 0u) {
        disarm(maintenance);
        return TAVRN_MAINTENANCE_GATED;
    }
    if (adaptive_enabled(maintenance) != 0 &&
        time_due(now_ms, maintenance->snapshot.next_topology_sample_ms)) {
        topology_changed = sample_topology(maintenance, now_ms);
    }
    if (maintenance->snapshot.pending != 0u) {
        return enqueue_pending(maintenance, now_ms);
    }
    if (topology_changed != 0u) {
        return TAVRN_MAINTENANCE_OK;
    }
    if (!time_due(now_ms, maintenance->snapshot.next_hello_due_ms)) {
        return TAVRN_MAINTENANCE_OK;
    }
    memset(&maintenance->snapshot.pending_hello, 0,
           sizeof(maintenance->snapshot.pending_hello));
    build_status = tavrn_router_build_ordinary_hello(
        maintenance->router, maintenance->snapshot.next_node_sequence,
        tavrn_gtt_known_remote_count(maintenance->gtt),
        &maintenance->snapshot.pending_hello);
    if (build_status != TAVRN_ROUTER_HELLO_OK) {
        return build_status == TAVRN_ROUTER_HELLO_GATED ?
            TAVRN_MAINTENANCE_GATED : TAVRN_MAINTENANCE_INVALID;
    }
    maintenance->snapshot.pending = 1u;
    maintenance->counters.pending = 1u;
    maintenance->counters.due++;
    return enqueue_pending(maintenance, now_ms);
}

tavrn_maintenance_status_t tavrn_maintenance_handle_rx_control(
    tavrn_maintenance_t *maintenance,
    const tavrn_rx_control_event_t *control_event, uint32_t now_ms)
{
    tavrn_adva_t origin;
    tavrn_maintenance_dedupe_entry_t *entry;
    tavrn_gtt_evidence_t evidence;
    tavrn_gtt_expiry_observe_status_t observe_status;
    uint16_t node_sequence;
    uint8_t known_remote_count;
    uint8_t duplicate;

    if (!maintenance_is_initialized(maintenance) || control_event == NULL) {
        return TAVRN_MAINTENANCE_INVALID;
    }
    /* A copied ordinary HELLO follows router-common's committed incarnation
     * state; its epoch must be reconciled before N=0 serial admission. */
    if (control_event->control.type == TAVRN_WIRE_HELLO &&
        synchronize_peer_epoch(maintenance, &control_event->transmitter.adva,
                               now_ms) != TAVRN_MAINTENANCE_OK) {
        return TAVRN_MAINTENANCE_INVALID;
    }
    if (!sid8_active(maintenance)) {
        return TAVRN_MAINTENANCE_IGNORED;
    }
    if (!ordinary_n0_sid8_hello(control_event)) {
        return TAVRN_MAINTENANCE_IGNORED;
    }
    if (!ordinary_hello_event(maintenance, control_event, &origin,
                               &node_sequence, &known_remote_count)) {
        maintenance->counters.rx_rejected++;
        return TAVRN_MAINTENANCE_RX_REJECTED;
    }
    (void)known_remote_count;
    entry = dedupe_slot(maintenance, &origin, node_sequence, now_ms, &duplicate);
    if (duplicate != 0u) {
        maintenance->counters.rx_duplicate++;
        return TAVRN_MAINTENANCE_RX_DUPLICATE;
    }
    if (entry == NULL) {
        maintenance->counters.rx_rejected++;
        maintenance->counters.dedupe_capacity++;
        return TAVRN_MAINTENANCE_RX_REJECTED;
    }
    memset(&evidence, 0, sizeof(evidence));
    evidence.identity = origin;
    evidence.serial = node_sequence;
    evidence.serial_present = 1u;
    evidence.hop_count = 1u;
    evidence.kind = TAVRN_GTT_EVIDENCE_LIVENESS;
    observe_status = tavrn_gtt_observe_with_provenance(
        maintenance->gtt, &evidence, TAVRN_GTT_PROVENANCE_DIRECT_HELLO, now_ms,
        NULL);
    if (observe_status != TAVRN_GTT_EXPIRY_OBSERVE_ADDED &&
        observe_status != TAVRN_GTT_EXPIRY_OBSERVE_REFRESHED) {
        maintenance->counters.rx_rejected++;
        return TAVRN_MAINTENANCE_RX_REJECTED;
    }
    remember_dedupe(entry, &origin, node_sequence, &maintenance->config, now_ms);
    (void)tavrn_maintenance_targeted_cancel_subject(
        maintenance, maintenance->router, maintenance->router->link, maintenance->gtt,
        &origin, now_ms);
    maintenance->counters.rx_unique++;
    return TAVRN_MAINTENANCE_RX_UNIQUE;
}

tavrn_maintenance_status_t tavrn_maintenance_observe_local_broadcast(
    tavrn_maintenance_t *maintenance, uint32_t accepted_at_ms)
{
    if (!maintenance_is_initialized(maintenance)) {
        return TAVRN_MAINTENANCE_INVALID;
    }
    if (!sid8_active(maintenance) || maintenance->snapshot.armed == 0u) {
        return TAVRN_MAINTENANCE_GATED;
    }
    if (adaptive_enabled(maintenance) == 0) {
        return TAVRN_MAINTENANCE_IGNORED;
    }
    if (maintenance->snapshot.pending != 0u) {
        /* This HELLO has not entered the link queue.  The accepted broadcast
         * satisfies this cadence cycle, so discard only the retained local
         * copy without consuming its node sequence. */
        clear_pending(maintenance);
    }
    maintenance->counters.local_broadcast_suppressed++;
    if (maintenance->local_broadcast_suppression_active != 0u) {
        return TAVRN_MAINTENANCE_OK;
    }
    advance_adaptive_interval(maintenance);
    maintenance->snapshot.next_hello_due_ms = accepted_at_ms +
        maintenance->snapshot.current_interval_ms;
    maintenance->local_broadcast_suppression_active = 1u;
    return TAVRN_MAINTENANCE_OK;
}

tavrn_maintenance_status_t tavrn_maintenance_snapshot(
    const tavrn_maintenance_t *maintenance,
    tavrn_maintenance_snapshot_t *snapshot_out)
{
    if (!maintenance_is_initialized(maintenance) || snapshot_out == NULL) {
        return TAVRN_MAINTENANCE_INVALID;
    }
    *snapshot_out = maintenance->snapshot;
    return TAVRN_MAINTENANCE_OK;
}

const tavrn_maintenance_counters_t *tavrn_maintenance_counters(
    const tavrn_maintenance_t *maintenance)
{
    return maintenance_is_initialized(maintenance) ? &maintenance->counters : NULL;
}

static tavrn_gtt_entry_t *maintenance_entry(tavrn_maintenance_t *maintenance,
                                            const tavrn_adva_t *identity)
{
    uint8_t index;

    if (!maintenance_is_initialized(maintenance) || identity == NULL) {
        return NULL;
    }
    for (index = 0u; index < TAVRN_GTT_CAPACITY; index++) {
        tavrn_gtt_entry_t *entry = &maintenance->gtt->storage->entries[index];

        if (entry->occupied != 0u && adva_equal(&entry->identity, identity)) {
            return entry;
        }
    }
    return NULL;
}

static const tavrn_gtt_entry_t *maintenance_const_entry(
    const tavrn_maintenance_t *maintenance, const tavrn_adva_t *identity)
{
    return maintenance_entry((tavrn_maintenance_t *)(void *)maintenance, identity);
}

static uint32_t expiry_interval(const tavrn_maintenance_t *maintenance)
{
    return maintenance->config.topology_sample_ms != 0u ?
        maintenance->config.topology_sample_ms : maintenance->config.hello_change_ms;
}

static int logical_subject_for_identity(const tavrn_maintenance_t *maintenance,
                                        const tavrn_adva_t *identity,
                                        const tavrn_gtt_entry_t *entry,
                                        tavrn_logical_id_t *subject_out)
{
    uint8_t matches = 0u;
    uint8_t index;
    tavrn_identity_width_t width;

    if (!maintenance_is_initialized(maintenance) || identity == NULL ||
        subject_out == NULL) {
        return 0;
    }
    width = maintenance->router->link->config.local_peer.logical_id.width;
    memset(subject_out, 0, sizeof(*subject_out));
    subject_out->width = width;
    if (width == TAVRN_IDENTITY_SID16) {
        subject_out->value = (uint16_t)identity->bytes[0] |
            ((uint16_t)identity->bytes[1] << 8);
        return subject_out->value != 0u && subject_out->value != 0xffffu;
    }
    if (width != TAVRN_IDENTITY_SID8 || identity->bytes[0] == 0u ||
        identity->bytes[0] == 0xffu) {
        return 0;
    }
    /* A departed subject has no live compressed context, but it also has no
     * demand; permit its zero-demand departure/purge bookkeeping. */
    if (entry != NULL && entry->departed != 0u) {
        subject_out->value = identity->bytes[0];
        return 1;
    }
    for (index = 0u; index < TAVRN_GTT_CAPACITY; index++) {
        const tavrn_gtt_entry_t *candidate =
            &maintenance->gtt->storage->entries[index];

        if (candidate->occupied != 0u && candidate->departed == 0u &&
            candidate->identity.bytes[0] == identity->bytes[0]) {
            matches++;
        }
    }
    if (matches != 1u) {
        return 0;
    }
    subject_out->value = identity->bytes[0];
    return 1;
}

tavrn_maintenance_demand_status_t tavrn_maintenance_demand_snapshot(
    const tavrn_maintenance_t *maintenance, const tavrn_adva_t *identity,
    uint32_t now_ms, tavrn_maintenance_demand_snapshot_t *snapshot_out)
{
    tavrn_maintenance_demand_snapshot_t snapshot;
    const tavrn_gtt_entry_t *entry;
    tavrn_logical_id_t subject;
    tavrn_aodv_subject_demand_snapshot_t aodv;
    tavrn_link_subject_demand_snapshot_t link;
    tavrn_router_subject_demand_snapshot_t router;

    if (snapshot_out != NULL) {
        memset(snapshot_out, 0, sizeof(*snapshot_out));
    }
    if (!maintenance_is_initialized(maintenance) || identity == NULL ||
        snapshot_out == NULL) {
        return TAVRN_MAINTENANCE_DEMAND_INVALID;
    }
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.identity = *identity;
    entry = maintenance_const_entry(maintenance, identity);
    if (entry == NULL) {
        snapshot.snapshot_available = 1u;
        *snapshot_out = snapshot;
        return TAVRN_MAINTENANCE_DEMAND_OK;
    }
    snapshot.gtt_revision = entry->revision;
    if (!logical_subject_for_identity(maintenance, identity, entry, &subject)) {
        return TAVRN_MAINTENANCE_DEMAND_UNAVAILABLE;
    }
    aodv = aodv_core_subject_demand_snapshot(maintenance->router->aodv, &subject,
                                              identity, now_ms);
    link = tavrn_link_v2_subject_demand_snapshot(maintenance->router->link, &subject,
                                                 identity, now_ms);
    router = tavrn_router_subject_demand_snapshot(maintenance->router, &subject,
                                                  identity, now_ms);
    if (aodv.snapshot_available == 0u || link.snapshot_available == 0u ||
        router.snapshot_available == 0u) {
        return TAVRN_MAINTENANCE_DEMAND_UNAVAILABLE;
    }
    snapshot.reason_mask = (uint16_t)(aodv.reason_mask | link.reason_mask |
                                      router.reason_mask);
    if (entry->application_requested != 0u) {
        snapshot.reason_mask |= TAVRN_MAINT_DEMAND_APPLICATION_REQUEST;
    }
    snapshot.valid_route_to_subject = aodv.valid_route_to_subject;
    snapshot.stage_zero_eligible = aodv.valid_route_to_subject;
    snapshot.snapshot_available = 1u;
    *snapshot_out = snapshot;
    return TAVRN_MAINTENANCE_DEMAND_OK;
}

static int direct_deadline_for(const tavrn_maintenance_t *maintenance,
                               const tavrn_gtt_entry_t *entry,
                               uint32_t *deadline_out)
{
    uint32_t interval_limit;
    uint32_t duration;

    if (maintenance == NULL || entry == NULL || deadline_out == NULL) {
        return 0;
    }
    interval_limit = saturating_multiply(maintenance->snapshot.current_interval_ms,
                                         TAVRN_MAINTENANCE_LIVENESS_FACTOR);
    duration = maintenance->snapshot.liveness_floor_ms > interval_limit ?
        maintenance->snapshot.liveness_floor_ms : interval_limit;
    if (duration == 0u || duration >= TAVRN_MAINTENANCE_HALF_RANGE) {
        return 0;
    }
    *deadline_out = entry->last_direct_evidence_ms + duration;
    return 1;
}

static uint32_t maintenance_next_generation(tavrn_maintenance_t *maintenance)
{
    uint32_t generation = maintenance->gtt->storage->next_generation + 1u;

    if (generation == 0u) {
        generation = 1u;
    }
    maintenance->gtt->storage->next_generation = generation;
    return generation;
}

tavrn_maintenance_checked_departure_status_t tavrn_maintenance_checked_local_departure(
    tavrn_maintenance_t *maintenance,
    const tavrn_gtt_departure_candidate_t *candidate, uint32_t now_ms,
    tavrn_gtt_expiry_snapshot_t *snapshot_out)
{
    tavrn_gtt_entry_t *entry;
    tavrn_gtt_expiry_snapshot_t snapshot;
    tavrn_maintenance_demand_snapshot_t demand;
    tavrn_maintenance_demand_status_t demand_status;
    uint32_t direct_deadline;
    uint32_t generation;

    if (snapshot_out != NULL) {
        memset(snapshot_out, 0, sizeof(*snapshot_out));
    }
    if (!maintenance_is_initialized(maintenance) || candidate == NULL) {
        return TAVRN_GTT_CHECKED_INVALID;
    }
    entry = maintenance_entry(maintenance, &candidate->identity);
    if (entry == NULL) {
        return TAVRN_GTT_CHECKED_NOT_FOUND;
    }
    if (tavrn_gtt_expiry_snapshot(maintenance->gtt, &candidate->identity, now_ms,
                                  &snapshot) != TAVRN_GTT_EXPIRY_QUERY_FOUND) {
        return TAVRN_GTT_CHECKED_INVALID;
    }
    if (snapshot_out != NULL) {
        *snapshot_out = snapshot;
    }
    if (candidate->revision != entry->revision) {
        snapshot.freshness = TAVRN_GTT_FRESHNESS_ACTIVE;
        if (snapshot_out != NULL) {
            *snapshot_out = snapshot;
        }
        return TAVRN_GTT_CHECKED_STALE_REVISION;
    }
    if (candidate->hard_deadline_ms != entry->hard_deadline_ms) {
        snapshot.freshness = TAVRN_GTT_FRESHNESS_ACTIVE;
        if (snapshot_out != NULL) {
            *snapshot_out = snapshot;
        }
        return TAVRN_GTT_CHECKED_INVALID;
    }
    if (adva_equal(&candidate->identity, &maintenance->gtt->config.local_identity)) {
        return TAVRN_GTT_CHECKED_SELF;
    }
    if (entry->departed != 0u) {
        return TAVRN_GTT_CHECKED_ALREADY_DEPARTED;
    }
    if (!time_due(now_ms, entry->hard_deadline_ms)) {
        /* A copied departure candidate represents the still-live membership
         * observation, not a request to expose an intermediate soft-stale
         * classification.  Rejection must leave that candidate value intact. */
        snapshot.freshness = TAVRN_GTT_FRESHNESS_ACTIVE;
        if (snapshot_out != NULL) {
            *snapshot_out = snapshot;
        }
        return TAVRN_GTT_CHECKED_NOT_HARD_EXPIRED;
    }
    demand_status = tavrn_maintenance_demand_snapshot(maintenance,
                                                       &candidate->identity,
                                                       now_ms, &demand);
    if (demand_status != TAVRN_MAINTENANCE_DEMAND_OK ||
        demand.snapshot_available == 0u) {
        return TAVRN_GTT_CHECKED_UNAVAILABLE;
    }
    if (demand.reason_mask != 0u) {
        return TAVRN_GTT_CHECKED_DEMAND_DEFERRED;
    }
    if (entry->direct != 0u) {
        if (!direct_deadline_for(maintenance, entry, &direct_deadline)) {
            if (snapshot_out != NULL) {
                memset(snapshot_out, 0, sizeof(*snapshot_out));
            }
            return TAVRN_GTT_CHECKED_INVALID;
        }
        if (!time_due(now_ms, direct_deadline)) {
            snapshot.freshness = TAVRN_GTT_FRESHNESS_ACTIVE;
            if (snapshot_out != NULL) {
                *snapshot_out = snapshot;
            }
            return TAVRN_GTT_CHECKED_DIRECT_DEADLINE_DEFERRED;
        }
    }
    entry->departed = 1u;
    entry->departed_deadline_ms = now_ms + maintenance->gtt->config.departed_retention_ms;
    entry->direct = 0u;
    entry->last_direct_evidence_ms = 0u;
    entry->application_requested = 0u;
    generation = maintenance_next_generation(maintenance);
    entry->revision = generation;
    entry->storage_generation = generation;
    maintenance->gtt->counters.departed++;
    if (tavrn_gtt_expiry_snapshot(maintenance->gtt, &candidate->identity, now_ms,
                                  &snapshot) != TAVRN_GTT_EXPIRY_QUERY_FOUND) {
        return TAVRN_GTT_CHECKED_INVALID;
    }
    if (snapshot_out != NULL) {
        *snapshot_out = snapshot;
    }
    return TAVRN_GTT_CHECKED_DEPARTED;
}

tavrn_maintenance_expiry_sweep_status_t tavrn_maintenance_sweep_expiry(
    tavrn_maintenance_t *maintenance, uint32_t now_ms,
    tavrn_maintenance_expiry_sweep_snapshot_t *snapshot_out)
{
    tavrn_maintenance_expiry_sweep_snapshot_t snapshot;
    uint8_t visited;
    uint8_t start_cursor;

    if (snapshot_out != NULL) {
        memset(snapshot_out, 0, sizeof(*snapshot_out));
    }
    if (!maintenance_is_initialized(maintenance) || snapshot_out == NULL ||
        expiry_interval(maintenance) == 0u) {
        return TAVRN_MAINTENANCE_EXPIRY_SWEEP_INVALID;
    }
    memset(&snapshot, 0, sizeof(snapshot));
    if (maintenance->expiry_schedule_initialized == 0u) {
        maintenance->expiry_next_due_ms = now_ms + expiry_interval(maintenance);
        maintenance->expiry_schedule_initialized = 1u;
    }
    snapshot.next_due_ms = maintenance->expiry_next_due_ms;
    snapshot.next_cursor = maintenance->expiry_cursor;
    if (!time_due(now_ms, maintenance->expiry_next_due_ms)) {
        *snapshot_out = snapshot;
        return TAVRN_MAINTENANCE_EXPIRY_SWEEP_OK;
    }
    snapshot.pass_performed = 1u;
    snapshot.received_evidence_count = maintenance->expiry_received_evidence_count;
    maintenance->expiry_received_evidence_count = 0u;
    start_cursor = maintenance->expiry_cursor;
    for (visited = 0u; visited < TAVRN_GTT_CAPACITY; visited++) {
        uint8_t slot = (uint8_t)((start_cursor + visited) %
                                  TAVRN_GTT_CAPACITY);
        tavrn_gtt_entry_t *entry = &maintenance->gtt->storage->entries[slot];

        if (entry->occupied == 0u) {
            continue;
        }
        if (entry->departed != 0u) {
            if (time_due(now_ms, entry->departed_deadline_ms)) {
                memset(entry, 0, sizeof(*entry));
                snapshot.purged_count++;
                continue;
            }
        }
        snapshot.trace_slots[snapshot.trace_count++] = slot;
        snapshot.maximum_copied_candidates = 1u;
        maintenance->expiry_cursor = (uint8_t)((slot + 1u) % TAVRN_GTT_CAPACITY);
        if (entry->departed != 0u) {
            continue;
        }
        if (adva_equal(&entry->identity, &maintenance->gtt->config.local_identity)) {
            continue;
        }
        if (time_due(now_ms, entry->hard_deadline_ms)) {
            tavrn_gtt_departure_candidate_t candidate;
            tavrn_maintenance_checked_departure_status_t status;
            uint8_t direct_subject = entry->direct;

            snapshot.hard_selected_count++;
            candidate.identity = entry->identity;
            candidate.revision = entry->revision;
            candidate.hard_deadline_ms = entry->hard_deadline_ms;
            status = tavrn_maintenance_checked_local_departure(maintenance,
                                                                &candidate, now_ms,
                                                                NULL);
            if (status == TAVRN_GTT_CHECKED_DEPARTED) {
                snapshot.departed_count++;
                if (direct_subject != 0u) {
                    tavrn_tc_metadata_status_t tc_status =
                        tavrn_maintenance_tc_on_direct_timeout_departure(
                            maintenance, &candidate.identity, now_ms);

                    if (tc_status == TAVRN_TC_METADATA_PREPARED ||
                        tc_status == TAVRN_TC_METADATA_RETAINED) {
                        snapshot.expiry_tc_control_count++;
                    }
                }
            } else if (status == TAVRN_GTT_CHECKED_DEMAND_DEFERRED ||
                       status == TAVRN_GTT_CHECKED_DIRECT_DEADLINE_DEFERRED) {
                snapshot.demand_deferred_count++;
                if (status == TAVRN_GTT_CHECKED_DEMAND_DEFERRED) {
                    tavrn_targeted_freshness_action_t action;
                    tavrn_targeted_freshness_status_t targeted_status;

                    memset(&action, 0, sizeof(action));
                    targeted_status = tavrn_maintenance_targeted_begin_stage0(
                        maintenance, maintenance->router, maintenance->router->link,
                        maintenance->gtt, &entry->identity, now_ms, &action);
                    if (targeted_status == TAVRN_TARGETED_FRESHNESS_OK &&
                        action.enqueued != 0u) {
                        snapshot.targeted_control_count++;
                    }
                }
            } else if (status == TAVRN_GTT_CHECKED_UNAVAILABLE ||
                       status == TAVRN_GTT_CHECKED_INVALID) {
                snapshot.unavailable_count++;
            }
        } else if (time_due(now_ms, entry->soft_deadline_ms)) {
            tavrn_metadata_candidate_t request;

            snapshot.soft_selected_count++;
            memset(&request, 0, sizeof(request));
            request.subject = entry->identity;
            request.subject_sid8 = entry->identity.bytes[0];
            request.ttl_bucket = 0u;
            request.freshness_request = 1u;
            request.kind = TAVRN_METADATA_SOFT_REQUEST;
            (void)tavrn_maintenance_metadata_create(&maintenance->tc_metadata,
                                                     &request, now_ms);
        }
    }
    maintenance->expiry_next_due_ms = now_ms + expiry_interval(maintenance);
    snapshot.next_due_ms = maintenance->expiry_next_due_ms;
    snapshot.next_cursor = maintenance->expiry_cursor;
    *snapshot_out = snapshot;
    return TAVRN_MAINTENANCE_EXPIRY_SWEEP_OK;
}

tavrn_maintenance_owner_pre_tick_result_t tavrn_maintenance_owner_pre_tick(
    tavrn_maintenance_t *maintenance, tavrn_maintenance_owner_pre_tick_input_t input,
    uint32_t now_ms)
{
    tavrn_maintenance_owner_pre_tick_result_t result;

    memset(&result, 0, sizeof(result));
    result.observe_status = TAVRN_GTT_EXPIRY_OBSERVE_INVALID;
    result.application_result.status = TAVRN_GTT_APPLICATION_INVALID;
    result.application_result.query_status = TAVRN_GTT_EXPIRY_QUERY_INVALID;
    if (!maintenance_is_initialized(maintenance) || input.evidence_present > 1u ||
        input.application_present > 1u) {
        return result;
    }
    if (input.evidence_present != 0u) {
        result.observe_present = 1u;
        result.observe_status = tavrn_gtt_observe_with_provenance(
            maintenance->gtt, &input.evidence, input.provenance, now_ms, NULL);
        if (result.observe_status != TAVRN_GTT_EXPIRY_OBSERVE_INVALID) {
            maintenance->expiry_received_evidence_count++;
        }
    }
    if (input.application_present != 0u) {
        result.application_present = 1u;
        input.application_command.now_ms = now_ms;
        input.application_command.query_time_ms = now_ms;
        result.application_result = tavrn_gtt_apply_application_command(
            maintenance->gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER,
            input.application_command);
    }
    return result;
}

tavrn_maintenance_owner_post_tick_result_t tavrn_maintenance_owner_post_tick(
    tavrn_maintenance_t *maintenance, uint32_t now_ms)
{
    tavrn_maintenance_owner_post_tick_result_t result;

    memset(&result, 0, sizeof(result));
    result.sweep_status = TAVRN_MAINTENANCE_EXPIRY_SWEEP_INVALID;
    result.maintenance_status = TAVRN_MAINTENANCE_INVALID;
    if (!maintenance_is_initialized(maintenance)) {
        return result;
    }
    if (!sid8_active(maintenance) || maintenance->snapshot.armed == 0u) {
        result.sweep_status = TAVRN_MAINTENANCE_EXPIRY_SWEEP_OK;
        result.maintenance_status = TAVRN_MAINTENANCE_GATED;
        result.maintenance = maintenance->snapshot;
        return result;
    }
    result.sweep_status = tavrn_maintenance_sweep_expiry(maintenance, now_ms,
                                                          &result.sweep);
    if (result.sweep_status != TAVRN_MAINTENANCE_EXPIRY_SWEEP_OK) {
        result.maintenance_status = TAVRN_MAINTENANCE_INVALID;
        return result;
    }
    result.maintenance_status = tavrn_maintenance_tick(maintenance, now_ms);
    if (tavrn_maintenance_snapshot(maintenance, &result.maintenance) !=
        TAVRN_MAINTENANCE_OK) {
        result.maintenance_status = TAVRN_MAINTENANCE_INVALID;
    }
    return result;
}

/* Targeted freshness is deliberately kept below the ordinary maintenance
 * surface.  Its state is fixed, local to this owner, and its only route view is
 * the copied router/AODV snapshot seam. */
static int targeted_owners_valid(const tavrn_maintenance_t *maintenance,
                                 const tavrn_router_t *router,
                                 const tavrn_link_v2_t *link,
                                 const tavrn_gtt_t *gtt)
{
    return maintenance_is_initialized(maintenance) && router != NULL &&
        link != NULL && gtt != NULL && maintenance->router == router &&
        maintenance->gtt == gtt && router->link == link;
}

static void targeted_clear_action(tavrn_targeted_freshness_action_t *action_out)
{
    if (action_out != NULL) {
        memset(action_out, 0, sizeof(*action_out));
    }
}

static uint16_t targeted_pdu_u16(const tavrn_validated_control_t *control,
                                 uint8_t offset)
{
    return (uint16_t)control->pdu[offset] |
        ((uint16_t)control->pdu[(uint8_t)(offset + 1u)] << 8);
}

static void targeted_pdu_put_u16(tavrn_validated_control_t *control,
                                  uint8_t offset, uint16_t value)
{
    control->pdu[offset] = (uint8_t)value;
    control->pdu[(uint8_t)(offset + 1u)] = (uint8_t)(value >> 8);
}

static tavrn_logical_id_t targeted_sid8(uint8_t value)
{
    tavrn_logical_id_t logical_id;

    logical_id.width = TAVRN_IDENTITY_SID8;
    logical_id.value = value;
    return logical_id;
}

static uint8_t targeted_ttl_for_route(const aodv_route_snapshot_t *route)
{
    uint16_t ttl;

    if (route == NULL || route->hop_count == 0u) {
        return 0u;
    }
    ttl = route->hop_count;
    return ttl > 15u ? 15u : (uint8_t)ttl;
}

static int targeted_control_valid(const tavrn_rx_control_event_t *event,
                                  const tavrn_maintenance_t *maintenance,
                                  uint8_t *request_out)
{
    const tavrn_validated_control_t *control;
    uint8_t request;

    if (event == NULL || maintenance == NULL || request_out == NULL) {
        return 0;
    }
    control = &event->control;
    request = control->pdu[5] == 0xb8u ? 1u : 0u;
    if (control->type != TAVRN_WIRE_HELLO || control->pdu_len != 20u ||
        control->pdu[0] != 0x54u || control->pdu[1] != 0x52u ||
        control->pdu[2] != 0x02u ||
         control->pdu[3] != maintenance->router->link->config.network_id ||
         control->pdu[4] != TAVRN_WIRE_HELLO ||
         (control->pdu[5] != 0xb8u && control->pdu[5] != 0xa8u) ||
         control->pdu[6] == 0u ||
         control->pdu[7] == 0u || control->pdu[7] == 0xffu ||
        control->pdu[8] == 0u || control->pdu[8] == 0xffu ||
         control->pdu[17] != 1u || control->pdu[18] == 0u ||
         control->pdu[18] == 0xffu ||
         (request != 0u && control->pdu[18] != control->pdu[8]) ||
         (request != 0u && control->pdu[19] != 0x01u) ||
        (request == 0u && (control->pdu[19] & 0x0fu) != 0u) ||
        event->transmitter.logical_id.width != TAVRN_IDENTITY_SID8 ||
        control->pdu[7] != maintenance->router->link->config.local_peer.logical_id.value) {
        return 0;
    }
    if ((control->pdu[6] & 0x0fu) == 0u &&
        memcmp(&control->pdu[9], event->transmitter.adva.bytes,
               TAVRN_ADVA_LEN) != 0) {
        return 0;
    }
    *request_out = request;
    return 1;
}

int tavrn_maintenance_is_targeted_control(
    const tavrn_rx_control_event_t *control_event)
{
    return control_event != NULL &&
        control_event->control.type == TAVRN_WIRE_HELLO &&
        control_event->control.pdu_len >= 6u &&
        (control_event->control.pdu[5] == 0xb8u ||
         control_event->control.pdu[5] == 0xa8u);
}

static tavrn_targeted_freshness_status_t targeted_record_receive_status(
    tavrn_maintenance_t *maintenance, tavrn_targeted_freshness_status_t status)
{
    if (maintenance != NULL) {
        maintenance->counters.targeted_rx++;
        if (status == TAVRN_TARGETED_FRESHNESS_INVALID) {
            maintenance->counters.targeted_invalid++;
        }
    }
    return status;
}

static tavrn_targeted_freshness_status_t targeted_record_owner_status(
    tavrn_maintenance_t *maintenance, tavrn_targeted_freshness_status_t status,
    const tavrn_targeted_freshness_action_t *action)
{
    if (maintenance != NULL) {
        maintenance->counters.targeted_owner_ticks++;
        if (status == TAVRN_TARGETED_FRESHNESS_INVALID) {
            maintenance->counters.targeted_invalid++;
        }
        if (action != NULL && action->enqueued != 0u) {
            maintenance->counters.targeted_owner_enqueued++;
        }
    }
    return status;
}

static tavrn_targeted_freshness_status_t targeted_record_scheduler_status(
    tavrn_maintenance_t *maintenance, tavrn_targeted_freshness_status_t status)
{
    if (maintenance != NULL) {
        maintenance->counters.targeted_scheduler_events++;
        if (status == TAVRN_TARGETED_FRESHNESS_INVALID) {
            maintenance->counters.targeted_invalid++;
        }
    }
    return status;
}

static tavrn_maintenance_dedupe_entry_t *targeted_dedupe_slot(
    tavrn_maintenance_t *maintenance, const tavrn_adva_t *origin,
    uint16_t node_sequence, uint8_t final_target, uint8_t subject,
    uint8_t request, uint32_t now_ms, uint8_t *duplicate_out)
{
    uint8_t index;

    *duplicate_out = 0u;
    purge_dedupe(maintenance, now_ms);
    for (index = 0u; index < TAVRN_MAINTENANCE_DEDUPE_CAPACITY; index++) {
        tavrn_maintenance_dedupe_entry_t *entry = &maintenance->dedupe[index];

        if (entry->valid != 0u && entry->targeted != 0u &&
            entry->node_sequence == node_sequence && entry->final_target == final_target &&
            entry->subject == subject && entry->request == request &&
            adva_equal(&entry->origin, origin)) {
            *duplicate_out = 1u;
            return entry;
        }
    }
    for (index = 0u; index < TAVRN_MAINTENANCE_DEDUPE_CAPACITY; index++) {
        if (maintenance->dedupe[index].valid == 0u) {
            return &maintenance->dedupe[index];
        }
    }
    return NULL;
}

static void targeted_remember_dedupe(tavrn_maintenance_dedupe_entry_t *entry,
                                     const tavrn_adva_t *origin,
                                     uint16_t node_sequence,
                                     uint8_t final_target, uint8_t subject,
                                     uint8_t request,
                                     const tavrn_maintenance_t *maintenance,
                                     uint32_t now_ms)
{
    memset(entry, 0, sizeof(*entry));
    entry->origin = *origin;
    entry->node_sequence = node_sequence;
    entry->final_target = final_target;
    entry->subject = subject;
    entry->targeted = 1u;
    entry->request = request;
    entry->expires_at_ms = now_ms + maintenance->config.hello_dedupe_ms;
    entry->valid = 1u;
}

static tavrn_targeted_freshness_context_t *targeted_context_from_index(
    tavrn_maintenance_t *maintenance, uint8_t index)
{
    if (maintenance == NULL) {
        return NULL;
    }
    if (index < TAVRN_TARGETED_FRESHNESS_CONTEXT_CAPACITY) {
        return &maintenance->targeted[index];
    }
    if (index >= TAVRN_TARGETED_RESPONSE_CONTEXT_INDEX_BASE &&
        (uint8_t)(index - TAVRN_TARGETED_RESPONSE_CONTEXT_INDEX_BASE) <
            TAVRN_TARGETED_RESPONSE_OBLIGATION_CAPACITY) {
        return &maintenance->targeted_response[
            index - TAVRN_TARGETED_RESPONSE_CONTEXT_INDEX_BASE];
    }
    return NULL;
}

static tavrn_targeted_freshness_context_t *targeted_context_at_ordinal(
    tavrn_maintenance_t *maintenance, uint8_t ordinal, uint8_t *index_out)
{
    if (ordinal < TAVRN_TARGETED_FRESHNESS_CONTEXT_CAPACITY) {
        *index_out = ordinal;
    } else {
        *index_out = (uint8_t)(TAVRN_TARGETED_RESPONSE_CONTEXT_INDEX_BASE +
            ordinal - TAVRN_TARGETED_FRESHNESS_CONTEXT_CAPACITY);
    }
    return targeted_context_from_index(maintenance, *index_out);
}

static tavrn_targeted_freshness_context_t *targeted_context_slot(
    tavrn_maintenance_t *maintenance, tavrn_targeted_freshness_work_kind_t kind,
    uint8_t *index_out)
{
    uint8_t index;

    if (kind == TAVRN_TARGETED_WORK_LOCAL_REQUEST) {
        for (index = 0u; index < TAVRN_TARGETED_FRESHNESS_CONTEXT_CAPACITY;
             index++) {
            if (maintenance->targeted[index].snapshot.valid == 0u) {
                *index_out = index;
                return &maintenance->targeted[index];
            }
        }
        return NULL;
    }
    for (index = 0u; index < TAVRN_TARGETED_RESPONSE_OBLIGATION_CAPACITY; index++) {
        if (maintenance->targeted_response[index].snapshot.valid == 0u) {
            *index_out = (uint8_t)(TAVRN_TARGETED_RESPONSE_CONTEXT_INDEX_BASE + index);
            return &maintenance->targeted_response[index];
        }
    }
    return NULL;
}

static tavrn_gtt_entry_t *targeted_entry_for_sid(tavrn_maintenance_t *maintenance,
                                                   uint8_t sid)
{
    uint8_t index;
    tavrn_gtt_entry_t *match = NULL;

    for (index = 0u; index < TAVRN_GTT_CAPACITY; index++) {
        tavrn_gtt_entry_t *entry = &maintenance->gtt->storage->entries[index];

        if (entry->occupied == 0u || entry->identity.bytes[0] != sid) {
            continue;
        }
        if (match != NULL) {
            return NULL;
        }
        match = entry;
    }
    return match;
}

static void targeted_subject_from_sid(tavrn_maintenance_t *maintenance,
                                      uint8_t sid, tavrn_adva_t *subject_out)
{
    tavrn_gtt_entry_t *entry = targeted_entry_for_sid(maintenance, sid);

    memset(subject_out, 0, sizeof(*subject_out));
    if (entry != NULL) {
        *subject_out = entry->identity;
        return;
    }
    /* An unresolved wire SID is still the only admissible relay/dedupe key. */
    subject_out->bytes[0] = sid;
}

/* A targeted response's metadata names the subject by SID8, while its full
 * inner origin identifies the evidence source.  Resolve the subject through
 * the request that caused this verification first; only an unambiguous known
 * GTT SID8 is a valid fallback for an unsolicited response. */
static int targeted_response_subject(tavrn_maintenance_t *maintenance,
                                     uint8_t sid, tavrn_adva_t *subject_out)
{
    uint8_t index;
    uint8_t matches = 0u;
    tavrn_adva_t subject;
    tavrn_gtt_entry_t *entry;

    if (maintenance == NULL || subject_out == NULL) {
        return 0;
    }
    memset(&subject, 0, sizeof(subject));
    for (index = 0u; index < TAVRN_TARGETED_FRESHNESS_CONTEXT_CAPACITY; index++) {
        const tavrn_targeted_freshness_context_t *context =
            &maintenance->targeted[index];

        if (context->snapshot.valid == 0u ||
            context->snapshot.work_kind != TAVRN_TARGETED_WORK_LOCAL_REQUEST ||
            context->control.pdu_len != 20u || context->control.pdu[18] != sid) {
            continue;
        }
        subject = context->snapshot.subject;
        matches++;
    }
    if (matches == 1u) {
        *subject_out = subject;
        return 1;
    }
    if (matches != 0u) {
        return 0;
    }
    entry = targeted_entry_for_sid(maintenance, sid);
    if (entry == NULL) {
        return 0;
    }
    *subject_out = entry->identity;
    return 1;
}

static tavrn_metadata_reservation_handle_t targeted_candidate_slot(
    tavrn_maintenance_t *maintenance, uint8_t context_index)
{
    tavrn_targeted_freshness_context_t *context =
        targeted_context_from_index(maintenance, context_index);

    if (context == NULL) {
        return 0u;
    }
    return tavrn_maintenance_metadata_reserve_delayed(
        maintenance, &context->snapshot.subject, context_index,
        context->snapshot.obligation_started_ms);
}

static void targeted_retire(tavrn_maintenance_t *maintenance, uint8_t index)
{
    tavrn_targeted_freshness_context_t *context;

    if (maintenance == NULL) {
        return;
    }
    context = targeted_context_from_index(maintenance, index);
    if (context == NULL) {
        return;
    }
    if (context->candidate_reservation != 0u) {
        tavrn_maintenance_metadata_release_delayed(maintenance,
                                                    context->candidate_reservation);
    }
    memset(context, 0, sizeof(*context));
}

static void targeted_enter_stage1(tavrn_targeted_freshness_context_t *context)
{
    if (context == NULL) {
        return;
    }
    /* Stage 0 is terminal at this boundary.  STAGE1_READY retains only the
     * bounded verification context; it owns no scheduler work, so its high
     * token must be available to another stage-0 context immediately. */
    context->snapshot.stage = TAVRN_TARGETED_STAGE1_READY;
    context->snapshot.token = BLE_MESH_TX_TOKEN_NONE;
    context->snapshot.queued = 0u;
    context->snapshot.in_flight = 0u;
    context->retry_pending = 0u;
}

static int targeted_is_rreq_stage(tavrn_targeted_freshness_stage_t stage)
{
    return stage == TAVRN_TARGETED_STAGE1_READY ||
        stage == TAVRN_TARGETED_STAGE1_WAIT_RESPONSE ||
        stage == TAVRN_TARGETED_STAGE2_READY ||
        stage == TAVRN_TARGETED_STAGE2_WAIT_RESPONSE ||
        stage == TAVRN_TARGETED_STAGE_WAIT_DIRECT_DEADLINE ||
        stage == TAVRN_TARGETED_STAGE_DEPARTED ||
        stage == TAVRN_TARGETED_STAGE_DEPARTURE_DEFERRED;
}

static int high_token_owned_by_targeted(const tavrn_maintenance_t *maintenance,
                                        uint16_t token)
{
    uint8_t index;

    if (maintenance == NULL || token < 0x8000u) {
        return 0;
    }
    for (index = 0u; index < TAVRN_TARGETED_FRESHNESS_CONTEXT_CAPACITY; index++) {
        const tavrn_targeted_freshness_context_t *context =
            &maintenance->targeted[index];

        if (context->snapshot.valid != 0u &&
            !targeted_is_rreq_stage(context->snapshot.stage) &&
            context->snapshot.token == token) {
            return 1;
        }
    }
    for (index = 0u; index < TAVRN_TARGETED_RESPONSE_OBLIGATION_CAPACITY; index++) {
        const tavrn_targeted_freshness_context_t *context =
            &maintenance->targeted_response[index];

        if (context->snapshot.valid != 0u &&
            !targeted_is_rreq_stage(context->snapshot.stage) &&
            context->snapshot.token == token) {
            return 1;
        }
    }
    return 0;
}

static int high_token_owned_by_verification(
    const tavrn_maintenance_t *maintenance, uint16_t token)
{
    uint8_t index;

    if (maintenance == NULL || token < 0x8000u) {
        return 0;
    }
    for (index = 0u; index < TAVRN_TARGETED_FRESHNESS_CONTEXT_CAPACITY; index++) {
        const tavrn_targeted_freshness_context_t *context =
            &maintenance->targeted[index];

        if (context->snapshot.valid != 0u &&
            targeted_is_rreq_stage(context->snapshot.stage) &&
            context->snapshot.token == token) {
            return 1;
        }
    }
    return 0;
}

static int high_token_owned_by_external(const tavrn_maintenance_t *maintenance,
                                        uint16_t token)
{
    return maintenance != NULL && token >= 0x8000u &&
        maintenance->external_high_token.valid != 0u &&
        maintenance->external_high_token.token == token;
}

static int high_token_targeted_owner_active(
    const tavrn_maintenance_t *maintenance)
{
    uint8_t index;

    if (maintenance == NULL) {
        return 0;
    }
    for (index = 0u; index < TAVRN_TARGETED_FRESHNESS_CONTEXT_CAPACITY; index++) {
        if (maintenance->targeted[index].snapshot.valid != 0u &&
            !targeted_is_rreq_stage(maintenance->targeted[index].snapshot.stage)) {
            return 1;
        }
    }
    for (index = 0u; index < TAVRN_TARGETED_RESPONSE_OBLIGATION_CAPACITY; index++) {
        if (maintenance->targeted_response[index].snapshot.valid != 0u &&
            !targeted_is_rreq_stage(
                maintenance->targeted_response[index].snapshot.stage)) {
            return 1;
        }
    }
    return 0;
}

static int high_token_verification_owner_active(
    const tavrn_maintenance_t *maintenance)
{
    uint8_t index;

    if (maintenance == NULL) {
        return 0;
    }
    for (index = 0u; index < TAVRN_TARGETED_FRESHNESS_CONTEXT_CAPACITY; index++) {
        if (maintenance->targeted[index].snapshot.valid != 0u &&
            targeted_is_rreq_stage(maintenance->targeted[index].snapshot.stage)) {
            return 1;
        }
    }
    return 0;
}

static uint16_t targeted_reserve_sequence(tavrn_maintenance_t *maintenance)
{
    return shared_sequence_reserve_targeted(maintenance);
}

static uint16_t targeted_allocate_high_token(tavrn_maintenance_t *maintenance,
                                              const tavrn_link_v2_t *link)
{
    uint32_t attempts;

    if (maintenance == NULL || link == NULL) {
        return BLE_MESH_TX_TOKEN_NONE;
    }
    for (attempts = 0u; attempts < 0x8000u; attempts++) {
        maintenance->targeted_next_token++;
        if (maintenance->targeted_next_token < 0x8000u) {
            maintenance->targeted_next_token = 0x8000u;
        }
        if (!high_token_owned_by_targeted(maintenance,
                                          maintenance->targeted_next_token) &&
            !high_token_owned_by_verification(maintenance,
                                              maintenance->targeted_next_token) &&
            !high_token_owned_by_external(maintenance,
                                          maintenance->targeted_next_token) &&
            !tavrn_link_v2_tracked_token_in_use(
                link, maintenance->targeted_next_token)) {
            return maintenance->targeted_next_token;
        }
    }
    return BLE_MESH_TX_TOKEN_NONE;
}

tavrn_maintenance_high_token_status_t tavrn_maintenance_high_token_reserve(
    tavrn_maintenance_t *maintenance, tavrn_link_v2_t *link,
    tavrn_maintenance_high_token_owner_t owner,
    tavrn_maintenance_high_token_purpose_t purpose,
    tavrn_maintenance_high_token_completion_fn completion, void *context,
    uint16_t *token_out)
{
    uint16_t token;

    if (token_out != NULL) {
        *token_out = BLE_MESH_TX_TOKEN_NONE;
    }
    if (maintenance == NULL || token_out == NULL || completion == NULL ||
        !targeted_owners_valid(maintenance, maintenance->router, link,
                               maintenance->gtt)) {
        return TAVRN_MAINTENANCE_HIGH_TOKEN_INVALID;
    }
    if (maintenance->external_high_token.valid != 0u) {
        return TAVRN_MAINTENANCE_HIGH_TOKEN_BUSY;
    }
    token = targeted_allocate_high_token(maintenance, link);
    if (token == BLE_MESH_TX_TOKEN_NONE) {
        return TAVRN_MAINTENANCE_HIGH_TOKEN_BUSY;
    }
    memset(&maintenance->external_high_token, 0,
           sizeof(maintenance->external_high_token));
    maintenance->external_high_token.owner = owner;
    maintenance->external_high_token.purpose = purpose;
    maintenance->external_high_token.token = token;
    maintenance->external_high_token.completion = completion;
    maintenance->external_high_token.context = context;
    maintenance->external_high_token.valid = 1u;
    *token_out = token;
    return TAVRN_MAINTENANCE_HIGH_TOKEN_OK;
}

tavrn_maintenance_high_token_status_t tavrn_maintenance_high_token_release(
    tavrn_maintenance_t *maintenance, tavrn_link_v2_t *link,
    tavrn_maintenance_high_token_owner_t owner,
    tavrn_maintenance_high_token_purpose_t purpose, uint16_t token)
{
    tavrn_maintenance_external_high_token_registration_t *registration;

    if (maintenance == NULL ||
        !targeted_owners_valid(maintenance, maintenance->router, link,
                               maintenance->gtt)) {
        return TAVRN_MAINTENANCE_HIGH_TOKEN_INVALID;
    }
    registration = &maintenance->external_high_token;
    if (registration->valid == 0u) {
        return TAVRN_MAINTENANCE_HIGH_TOKEN_NOT_FOUND;
    }
    if (registration->owner != owner || registration->purpose != purpose ||
        registration->token != token) {
        return TAVRN_MAINTENANCE_HIGH_TOKEN_MISMATCH;
    }
    if (tavrn_link_v2_tracked_token_in_use(link, token)) {
        return TAVRN_MAINTENANCE_HIGH_TOKEN_BUSY;
    }
    memset(registration, 0, sizeof(*registration));
    return TAVRN_MAINTENANCE_HIGH_TOKEN_OK;
}

static void targeted_build_request(tavrn_validated_control_t *control,
                                   const tavrn_maintenance_t *maintenance,
                                   const aodv_route_snapshot_t *route,
                                   uint8_t subject, uint16_t sequence)
{
    memset(control, 0, sizeof(*control));
    control->type = TAVRN_WIRE_HELLO;
    control->pdu_len = 20u;
    control->pdu[0] = 0x54u;
    control->pdu[1] = 0x52u;
    control->pdu[2] = 0x02u;
    control->pdu[3] = maintenance->router->link->config.network_id;
    control->pdu[4] = TAVRN_WIRE_HELLO;
    control->pdu[5] = 0xb8u;
    control->pdu[6] = (uint8_t)(targeted_ttl_for_route(route) << 4);
    control->pdu[7] = (uint8_t)route->next_hop.logical_id.value;
    control->pdu[8] = subject;
    memcpy(&control->pdu[9], maintenance->router->link->config.local_peer.adva.bytes,
           TAVRN_ADVA_LEN);
    targeted_pdu_put_u16(control, 15u, sequence);
    control->pdu[17] = 1u;
    control->pdu[18] = subject;
    control->pdu[19] = 0x01u;
}

static void targeted_build_response(tavrn_validated_control_t *control,
                                    const tavrn_maintenance_t *maintenance,
                                    const aodv_route_snapshot_t *route,
                                    uint8_t requester, uint8_t subject,
                                    uint16_t sequence, uint8_t bucket)
{
    memset(control, 0, sizeof(*control));
    control->type = TAVRN_WIRE_HELLO;
    control->pdu_len = 20u;
    control->pdu[0] = 0x54u;
    control->pdu[1] = 0x52u;
    control->pdu[2] = 0x02u;
    control->pdu[3] = maintenance->router->link->config.network_id;
    control->pdu[4] = TAVRN_WIRE_HELLO;
    control->pdu[5] = 0xa8u;
    control->pdu[6] = (uint8_t)(targeted_ttl_for_route(route) << 4);
    control->pdu[7] = (uint8_t)route->next_hop.logical_id.value;
    control->pdu[8] = requester;
    memcpy(&control->pdu[9], maintenance->router->link->config.local_peer.adva.bytes,
           TAVRN_ADVA_LEN);
    targeted_pdu_put_u16(control, 15u, sequence);
    control->pdu[17] = 1u;
    control->pdu[18] = subject;
    control->pdu[19] = (uint8_t)(bucket << 4);
}

static int targeted_relay_control(tavrn_validated_control_t *control,
                                  const tavrn_direct_peer_t *receiver)
{
    uint8_t ttl;
    uint8_t hops;

    if (control == NULL || receiver == NULL || control->pdu_len != 20u) {
        return 0;
    }
    ttl = (uint8_t)(control->pdu[6] >> 4);
    hops = (uint8_t)(control->pdu[6] & 0x0fu);
    if (ttl == 0u || hops == 15u || receiver->logical_id.width != TAVRN_IDENTITY_SID8) {
        return 0;
    }
    control->pdu[6] = (uint8_t)(((ttl - 1u) << 4) | (hops + 1u));
    control->pdu[7] = (uint8_t)receiver->logical_id.value;
    return 1;
}

static int targeted_route_to_sid(const tavrn_router_t *router, uint8_t sid,
                                 uint32_t now_ms, aodv_route_snapshot_t *route_out)
{
    tavrn_logical_id_t destination = targeted_sid8(sid);

    return tavrn_router_route_to_subject(router, &destination, now_ms, route_out) ==
        TAVRN_ROUTER_ROUTE_OK;
}

static int targeted_local_eligible(tavrn_maintenance_t *maintenance,
                                   const tavrn_adva_t *subject, uint32_t now_ms,
                                   aodv_route_snapshot_t *route_out)
{
    tavrn_gtt_entry_t *entry;
    tavrn_maintenance_demand_snapshot_t demand;
    tavrn_logical_id_t logical_subject;

    entry = maintenance_entry(maintenance, subject);
    if (entry == NULL || entry->departed != 0u ||
        !time_due(now_ms + 1u, entry->hard_deadline_ms) ||
        !logical_subject_for_identity(maintenance, subject, entry, &logical_subject) ||
        tavrn_maintenance_demand_snapshot(maintenance, subject, now_ms, &demand) !=
            TAVRN_MAINTENANCE_DEMAND_OK ||
        demand.snapshot_available == 0u || demand.reason_mask == 0u) {
        return 0;
    }
    return tavrn_router_route_to_subject(maintenance->router, &logical_subject,
                                         now_ms, route_out) == TAVRN_ROUTER_ROUTE_OK;
}

static void targeted_fill_action(const tavrn_targeted_freshness_context_t *context,
                                 uint8_t index,
                                 tavrn_targeted_freshness_action_t *action_out)
{
    if (action_out == NULL || context == NULL) {
        return;
    }
    action_out->control = context->control;
    action_out->token = context->snapshot.token;
    action_out->context_index = index;
    action_out->enqueued = context->snapshot.queued;
}

static tavrn_targeted_freshness_status_t targeted_admit(
    tavrn_maintenance_t *maintenance, uint8_t context_index, uint32_t now_ms,
    tavrn_targeted_freshness_action_t *action_out)
{
    tavrn_targeted_freshness_context_t *context =
        targeted_context_from_index(maintenance, context_index);
    ble_mesh_tx_token_t evicted = BLE_MESH_TX_TOKEN_NONE;
    tavrn_link_send_status_t status;

    if (context == NULL) {
        return TAVRN_TARGETED_FRESHNESS_INVALID;
    }
    targeted_fill_action(context, context_index, action_out);
    status = tavrn_link_v2_send_tracked_control(
        maintenance->router->link, &context->control, &context->snapshot.receiver,
        context->snapshot.token, now_ms, &evicted);
    if (evicted >= 0x8000u) {
        tavrn_maintenance_high_token_dispatch_result_t eviction;

        (void)tavrn_maintenance_high_token_evicted(
            maintenance, maintenance->router, maintenance->router->link, evicted,
            now_ms, &eviction);
    }
    if (status == TAVRN_LINK_SEND_OK) {
        context->snapshot.queued = 1u;
        context->snapshot.in_flight = 0u;
        context->retry_pending = 0u;
        targeted_fill_action(context, context_index, action_out);
        return TAVRN_TARGETED_FRESHNESS_OK;
    }
    if (status == TAVRN_LINK_SEND_BUSY || status == TAVRN_LINK_SEND_NO_SLOT) {
        return TAVRN_TARGETED_FRESHNESS_BUSY;
    }
    return TAVRN_TARGETED_FRESHNESS_INVALID;
}

static tavrn_targeted_freshness_status_t targeted_prepare_context(
    tavrn_maintenance_t *maintenance, tavrn_targeted_freshness_work_kind_t kind,
    tavrn_targeted_freshness_stage_t stage, const tavrn_adva_t *origin,
    const tavrn_adva_t *subject, const tavrn_direct_peer_t *receiver,
    const tavrn_validated_control_t *control, uint16_t sequence,
    uint8_t request_bucket, uint8_t response_bucket, uint32_t started_ms,
    uint32_t not_before_ms, uint8_t *index_out)
{
    tavrn_targeted_freshness_context_t *context;
    uint8_t index;
    uint16_t token;

    context = targeted_context_slot(maintenance, kind, &index);
    if (context == NULL) {
        return TAVRN_TARGETED_FRESHNESS_DEFERRED;
    }
    token = BLE_MESH_TX_TOKEN_NONE;
    if (stage != TAVRN_TARGETED_STAGE1_READY) {
        token = targeted_allocate_high_token(maintenance, maintenance->router->link);
        if (token == BLE_MESH_TX_TOKEN_NONE) {
            return TAVRN_TARGETED_FRESHNESS_BUSY;
        }
    }
    memset(context, 0, sizeof(*context));
    context->snapshot.origin = *origin;
    context->snapshot.subject = *subject;
    context->snapshot.receiver = *receiver;
    context->snapshot.work_kind = kind;
    context->snapshot.stage = stage;
    context->snapshot.obligation_started_ms = started_ms;
    context->snapshot.obligation_deadline_ms = started_ms +
        maintenance->config.hello_dedupe_ms;
    context->snapshot.not_before_ms = not_before_ms;
    context->snapshot.node_sequence = sequence;
    context->snapshot.token = token;
    context->snapshot.request_bucket = request_bucket;
    context->snapshot.response_bucket = response_bucket;
    context->snapshot.valid = 1u;
    context->control = *control;
    if (kind == TAVRN_TARGETED_WORK_LOCAL_REQUEST) {
        tavrn_gtt_entry_t *entry = maintenance_entry(maintenance, subject);

        if (entry == NULL) {
            memset(context, 0, sizeof(*context));
            return TAVRN_TARGETED_FRESHNESS_DEFERRED;
        }
        context->verification_expected_gtt_revision = entry->revision;
        context->verification_retained_hop = entry->hop_count;
        context->verification_direct_subject = entry->direct;
    }
    *index_out = index;
    return TAVRN_TARGETED_FRESHNESS_OK;
}

tavrn_targeted_freshness_status_t tavrn_maintenance_targeted_begin_stage0(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, tavrn_gtt_t *gtt, const tavrn_adva_t *subject,
    uint32_t now_ms, tavrn_targeted_freshness_action_t *action_out)
{
    aodv_route_snapshot_t route;
    tavrn_gtt_entry_t *entry;
    tavrn_maintenance_demand_snapshot_t demand;
    tavrn_logical_id_t logical_subject;
    tavrn_targeted_freshness_context_t *existing;
    tavrn_validated_control_t control;
    uint8_t index;
    uint8_t context_index;
    uint16_t sequence;
    tavrn_targeted_freshness_status_t status;

    targeted_clear_action(action_out);
    if (!targeted_owners_valid(maintenance, router, link, gtt) || subject == NULL ||
        action_out == NULL) {
        return TAVRN_TARGETED_FRESHNESS_INVALID;
    }
    entry = maintenance_entry(maintenance, subject);
    if (entry == NULL || entry->departed != 0u ||
        !time_due(now_ms + 1u, entry->hard_deadline_ms) ||
        !logical_subject_for_identity(maintenance, subject, entry, &logical_subject) ||
        tavrn_maintenance_demand_snapshot(maintenance, subject, now_ms, &demand) !=
            TAVRN_MAINTENANCE_DEMAND_OK || demand.snapshot_available == 0u ||
        demand.reason_mask == 0u) {
        return TAVRN_TARGETED_FRESHNESS_DEFERRED;
    }
    for (index = 0u; index < TAVRN_TARGETED_FRESHNESS_CONTEXT_CAPACITY; index++) {
        existing = &maintenance->targeted[index];
        if (existing->snapshot.valid != 0u &&
            existing->snapshot.work_kind == TAVRN_TARGETED_WORK_LOCAL_REQUEST &&
            adva_equal(&existing->snapshot.subject, subject)) {
            targeted_fill_action(existing, index, action_out);
            return existing->snapshot.stage == TAVRN_TARGETED_STAGE1_READY ?
                TAVRN_TARGETED_FRESHNESS_DEFERRED : TAVRN_TARGETED_FRESHNESS_OK;
        }
    }
    if (tavrn_router_route_to_subject(router, &logical_subject, now_ms, &route) !=
        TAVRN_ROUTER_ROUTE_OK) {
        tavrn_direct_peer_t none;

        memset(&none, 0, sizeof(none));
        memset(&control, 0, sizeof(control));
        status = targeted_prepare_context(
            maintenance, TAVRN_TARGETED_WORK_LOCAL_REQUEST,
            TAVRN_TARGETED_STAGE1_READY, &router->link->config.local_peer.adva,
            subject, &none, &control, 0u, 0u, 0u, now_ms, 0u, &context_index);
        if (status == TAVRN_TARGETED_FRESHNESS_OK) {
            targeted_fill_action(targeted_context_from_index(maintenance, context_index),
                                 context_index, action_out);
        }
        return TAVRN_TARGETED_FRESHNESS_DEFERRED;
    }
    sequence = targeted_reserve_sequence(maintenance);
    targeted_build_request(&control, maintenance, &route, (uint8_t)logical_subject.value,
                           sequence);
    status = targeted_prepare_context(
        maintenance, TAVRN_TARGETED_WORK_LOCAL_REQUEST, TAVRN_TARGETED_STAGE0_READY,
        &router->link->config.local_peer.adva, subject, &route.next_hop, &control,
        sequence, 0u, 0u, now_ms, now_ms, &context_index);
    if (status != TAVRN_TARGETED_FRESHNESS_OK) {
        return status;
    }
    return targeted_admit(maintenance, context_index, now_ms, action_out);
}

static uint32_t targeted_response_delay(const tavrn_maintenance_t *maintenance,
                                        const tavrn_adva_t *requester,
                                        uint16_t sequence, uint8_t final_target,
                                        uint8_t subject)
{
    uint32_t hash = 2166136261u;
    uint32_t span;
    uint8_t sequence_bytes[2];
    uint8_t index;
    uint8_t network = maintenance->router->link->config.network_id;

#define TARGETED_FNV_FEED(byte_value) \
    do { hash ^= (uint8_t)(byte_value); hash *= 16777619u; } while (0)
    TARGETED_FNV_FEED(network);
    for (index = 0u; index < TAVRN_ADVA_LEN; index++) {
        TARGETED_FNV_FEED(requester->bytes[index]);
    }
    sequence_bytes[0] = (uint8_t)sequence;
    sequence_bytes[1] = (uint8_t)(sequence >> 8);
    TARGETED_FNV_FEED(sequence_bytes[0]);
    TARGETED_FNV_FEED(sequence_bytes[1]);
    TARGETED_FNV_FEED(final_target);
    TARGETED_FNV_FEED(subject);
#undef TARGETED_FNV_FEED
    span = maintenance->config.freshness_response_max_ms -
        maintenance->config.freshness_response_min_ms + 1u;
    return maintenance->config.freshness_response_min_ms + hash % span;
}

static uint8_t targeted_remaining_bucket(const tavrn_gtt_entry_t *entry,
                                         uint32_t now_ms)
{
    uint32_t remaining;

    if (entry == NULL || entry->departed != 0u || time_due(now_ms,
                                                           entry->hard_deadline_ms)) {
        return 0u;
    }
    remaining = entry->hard_deadline_ms - now_ms;
    remaining /= 20000u;
    return remaining > 15u ? 15u : (uint8_t)remaining;
}

static void targeted_suppress_delayed_response(tavrn_maintenance_t *maintenance,
                                                uint8_t final_target,
                                                uint8_t subject)
{
    uint8_t index;

    for (index = 0u; index < TAVRN_TARGETED_FRESHNESS_CONTEXT_CAPACITY +
         TAVRN_TARGETED_RESPONSE_OBLIGATION_CAPACITY; index++) {
        uint8_t context_index;
        tavrn_targeted_freshness_context_t *context =
            targeted_context_at_ordinal(maintenance, index, &context_index);

        if (context->snapshot.valid != 0u &&
            context->snapshot.work_kind ==
                TAVRN_TARGETED_WORK_INTERMEDIARY_RESPONSE &&
            context->snapshot.queued == 0u && context->snapshot.in_flight == 0u &&
            adva_equal(&context->snapshot.origin,
                       &maintenance->router->link->config.local_peer.adva) &&
            context->control.pdu[8] == final_target &&
            context->control.pdu[18] == subject) {
            targeted_retire(maintenance, context_index);
        }
    }
}

static tavrn_targeted_freshness_status_t targeted_receive_request(
    tavrn_maintenance_t *maintenance, const tavrn_targeted_freshness_rx_input_t *input,
    const tavrn_adva_t *requester, uint16_t request_sequence,
    tavrn_maintenance_dedupe_entry_t *dedupe, uint32_t now_ms,
    tavrn_targeted_freshness_action_t *action_out)
{
    const tavrn_validated_control_t *received = &input->control_event.control;
    uint8_t final_target = received->pdu[8];
    uint8_t subject_sid = received->pdu[18];
    tavrn_gtt_entry_t *subject_entry;
    tavrn_adva_t subject;
    aodv_route_snapshot_t route;
    tavrn_validated_control_t control;
    tavrn_targeted_freshness_status_t status;
    uint8_t context_index;
    tavrn_metadata_reservation_handle_t candidate_reservation = 0u;
    uint16_t sequence;

    targeted_subject_from_sid(maintenance, subject_sid, &subject);
    if (final_target == maintenance->router->link->config.local_peer.logical_id.value) {
        uint8_t self_bucket = (uint8_t)(maintenance->gtt->config.hard_expiry_ms / 20000u);

        if (!targeted_route_to_sid(maintenance->router, requester->bytes[0], now_ms,
                                   &route)) {
            return TAVRN_TARGETED_FRESHNESS_DROPPED;
        }
        if (self_bucket > 15u) {
            self_bucket = 15u;
        }
        sequence = targeted_reserve_sequence(maintenance);
        targeted_build_response(&control, maintenance, &route, requester->bytes[0],
                                subject_sid, sequence, self_bucket);
        status = targeted_prepare_context(
            maintenance, TAVRN_TARGETED_WORK_TARGET_RESPONSE,
            TAVRN_TARGETED_STAGE0_READY,
            &maintenance->router->link->config.local_peer.adva, &subject,
            &route.next_hop, &control, sequence, 0u, self_bucket, now_ms, now_ms,
            &context_index);
    } else {
        uint8_t bucket;

        subject_entry = targeted_entry_for_sid(maintenance, subject_sid);
        bucket = targeted_remaining_bucket(subject_entry, now_ms);
        if (subject_entry != NULL && bucket != 0u) {
            if (!targeted_route_to_sid(maintenance->router, requester->bytes[0], now_ms,
                                       &route)) {
                return TAVRN_TARGETED_FRESHNESS_DROPPED;
            }
            sequence = targeted_reserve_sequence(maintenance);
            targeted_build_response(&control, maintenance, &route, requester->bytes[0],
                                    subject_sid, sequence, bucket);
            status = targeted_prepare_context(
                maintenance, TAVRN_TARGETED_WORK_INTERMEDIARY_RESPONSE,
                TAVRN_TARGETED_STAGE0_READY,
                &maintenance->router->link->config.local_peer.adva, &subject,
                &route.next_hop, &control, sequence, 0u, bucket, now_ms,
                now_ms + targeted_response_delay(maintenance, requester,
                                                 request_sequence, final_target,
                                                 subject_sid),
                &context_index);
            if (status == TAVRN_TARGETED_FRESHNESS_OK) {
                candidate_reservation = targeted_candidate_slot(maintenance,
                                                                context_index);
                if (candidate_reservation == 0u) {
                    targeted_retire(maintenance, context_index);
                    return TAVRN_TARGETED_FRESHNESS_BUSY;
                }
                targeted_context_from_index(maintenance, context_index)
                    ->candidate_reservation = candidate_reservation;
            }
        } else if (subject_entry != NULL && subject_entry->departed == 0u &&
                   !time_due(now_ms, subject_entry->hard_deadline_ms)) {
            /* An active zero bucket is normatively silent, not a relay cue. */
            return TAVRN_TARGETED_FRESHNESS_DROPPED;
        } else {
            if (!targeted_route_to_sid(maintenance->router, final_target, now_ms,
                                       &route) ||
                route.next_hop.logical_id.width != TAVRN_IDENTITY_SID8) {
                return TAVRN_TARGETED_FRESHNESS_DROPPED;
            }
            control = *received;
            if (!targeted_relay_control(&control, &route.next_hop)) {
                return TAVRN_TARGETED_FRESHNESS_DROPPED;
            }
            status = targeted_prepare_context(
                maintenance, TAVRN_TARGETED_WORK_REQUEST_RELAY,
                TAVRN_TARGETED_STAGE0_READY, requester, &subject, &route.next_hop,
                &control, request_sequence, 0u, 0u, now_ms, now_ms,
                &context_index);
        }
    }
    if (status != TAVRN_TARGETED_FRESHNESS_OK) {
        return status;
    }
    targeted_remember_dedupe(dedupe, requester, request_sequence, final_target,
                             subject_sid, 1u, maintenance, now_ms);
    if (targeted_context_from_index(maintenance, context_index)->snapshot.work_kind ==
        TAVRN_TARGETED_WORK_INTERMEDIARY_RESPONSE) {
        targeted_fill_action(targeted_context_from_index(maintenance, context_index),
                             context_index, action_out);
        return TAVRN_TARGETED_FRESHNESS_OK;
    }
    return targeted_admit(maintenance, context_index, now_ms, action_out);
}

static void targeted_complete_local_request(tavrn_maintenance_t *maintenance,
                                            tavrn_link_v2_t *link,
                                            const tavrn_adva_t *subject)
{
    uint8_t index;

    for (index = 0u; index < TAVRN_TARGETED_FRESHNESS_CONTEXT_CAPACITY +
         TAVRN_TARGETED_RESPONSE_OBLIGATION_CAPACITY; index++) {
        uint8_t context_index;
        tavrn_targeted_freshness_context_t *context =
            targeted_context_at_ordinal(maintenance, index, &context_index);
        uint8_t canceled = 0u;

        if (context->snapshot.valid == 0u ||
            context->snapshot.work_kind != TAVRN_TARGETED_WORK_LOCAL_REQUEST ||
            !adva_equal(&context->snapshot.subject, subject)) {
            continue;
        }
        if (context->snapshot.queued != 0u) {
            (void)tavrn_link_v2_cancel_queued_tracked_control(
                link, &context->control, context->snapshot.token, &canceled);
        }
        if (context->snapshot.in_flight != 0u ||
            (context->snapshot.queued != 0u && canceled == 0u)) {
            context->snapshot.stage = TAVRN_TARGETED_STAGE_CANCELED_TOMBSTONE;
            context->snapshot.queued = 0u;
            continue;
        }
        targeted_retire(maintenance, context_index);
    }
}

static tavrn_targeted_freshness_status_t targeted_admit_terminal_response(
    tavrn_maintenance_t *maintenance, tavrn_link_v2_t *link,
    const tavrn_adva_t *origin, uint16_t sequence,
    const tavrn_validated_control_t *received,
    tavrn_maintenance_dedupe_entry_t *dedupe, uint32_t now_ms)
{
    tavrn_gtt_evidence_t evidence;
    tavrn_gtt_expiry_observe_status_t observe_status;
    tavrn_adva_t subject;

    if (!targeted_response_subject(maintenance, received->pdu[18], &subject)) {
        return TAVRN_TARGETED_FRESHNESS_DROPPED;
    }

    memset(&evidence, 0, sizeof(evidence));
    evidence.identity = subject;
    /* Only a target-self response has a subject-owned node sequence.  An
     * intermediary's sequence is its own and must never become the subject's
     * GTT serial. */
    if (origin->bytes[0] == received->pdu[18]) {
        evidence.serial = sequence;
        evidence.serial_present = 1u;
    }
    evidence.hop_count = (uint8_t)(received->pdu[6] & 0x0fu);
    evidence.kind = TAVRN_GTT_EVIDENCE_LIVENESS;
    observe_status = tavrn_gtt_observe_with_provenance(
        maintenance->gtt, &evidence, TAVRN_GTT_PROVENANCE_IMPORTED_METADATA,
        now_ms, NULL);
    if (observe_status != TAVRN_GTT_EXPIRY_OBSERVE_INVALID) {
        maintenance->expiry_received_evidence_count++;
    }
    if (observe_status != TAVRN_GTT_EXPIRY_OBSERVE_ADDED &&
        observe_status != TAVRN_GTT_EXPIRY_OBSERVE_REFRESHED) {
        return TAVRN_TARGETED_FRESHNESS_DROPPED;
    }
    targeted_remember_dedupe(dedupe, origin, sequence, received->pdu[8],
                             received->pdu[18], 0u, maintenance, now_ms);
    targeted_complete_local_request(maintenance, link, &subject);
    return TAVRN_TARGETED_FRESHNESS_OK;
}

static tavrn_targeted_freshness_status_t targeted_receive_response(
    tavrn_maintenance_t *maintenance, const tavrn_targeted_freshness_rx_input_t *input,
    const tavrn_adva_t *origin, uint16_t sequence,
    tavrn_maintenance_dedupe_entry_t *dedupe, uint32_t now_ms,
    tavrn_targeted_freshness_action_t *action_out)
{
    const tavrn_validated_control_t *received = &input->control_event.control;
    tavrn_adva_t subject;
    aodv_route_snapshot_t route;
    tavrn_validated_control_t control;
    tavrn_targeted_freshness_status_t status;
    uint8_t context_index;

    if (received->pdu[8] == maintenance->router->link->config.local_peer.logical_id.value) {
        return targeted_admit_terminal_response(maintenance,
                                                maintenance->router->link, origin,
                                                sequence, received, dedupe, now_ms);
    }
    targeted_subject_from_sid(maintenance, received->pdu[18], &subject);
    if (!targeted_route_to_sid(maintenance->router, received->pdu[8], now_ms,
                               &route) ||
        route.next_hop.logical_id.width != TAVRN_IDENTITY_SID8) {
        return TAVRN_TARGETED_FRESHNESS_DROPPED;
    }
    control = *received;
    if (!targeted_relay_control(&control, &route.next_hop)) {
        return TAVRN_TARGETED_FRESHNESS_DROPPED;
    }
    status = targeted_prepare_context(
        maintenance, TAVRN_TARGETED_WORK_RESPONSE_RELAY,
        TAVRN_TARGETED_STAGE0_READY, origin, &subject, &route.next_hop, &control,
        sequence, 0u, (uint8_t)(received->pdu[19] >> 4), now_ms, now_ms,
        &context_index);
    if (status != TAVRN_TARGETED_FRESHNESS_OK) {
        return status;
    }
    targeted_remember_dedupe(dedupe, origin, sequence, received->pdu[8],
                             received->pdu[18], 0u, maintenance, now_ms);
    targeted_suppress_delayed_response(maintenance, received->pdu[8],
                                       received->pdu[18]);
    return targeted_admit(maintenance, context_index, now_ms, action_out);
}

tavrn_targeted_freshness_status_t tavrn_maintenance_targeted_receive(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, tavrn_gtt_t *gtt,
    const tavrn_targeted_freshness_rx_input_t *input, uint32_t now_ms,
    tavrn_targeted_freshness_action_t *action_out)
{
    const tavrn_validated_control_t *control;
    tavrn_adva_t origin;
    tavrn_maintenance_dedupe_entry_t *dedupe;
    uint16_t sequence;
    uint8_t request;
    uint8_t duplicate;

    targeted_clear_action(action_out);
    if (!targeted_owners_valid(maintenance, router, link, gtt) || input == NULL ||
        action_out == NULL) {
        return targeted_record_receive_status(maintenance,
                                              TAVRN_TARGETED_FRESHNESS_INVALID);
    }
    if (!targeted_control_valid(&input->control_event, maintenance, &request)) {
        return targeted_record_receive_status(maintenance,
                                              TAVRN_TARGETED_FRESHNESS_INVALID);
    }
    control = &input->control_event.control;
    memcpy(origin.bytes, &control->pdu[9], TAVRN_ADVA_LEN);
    sequence = targeted_pdu_u16(control, 15u);
    dedupe = targeted_dedupe_slot(maintenance, &origin, sequence, control->pdu[8],
                                   control->pdu[18], request, now_ms,
                                   &duplicate);
    if (duplicate != 0u) {
        return targeted_record_receive_status(maintenance,
                                              TAVRN_TARGETED_FRESHNESS_DUPLICATE);
    }
    if (dedupe == NULL) {
        return targeted_record_receive_status(maintenance,
                                              TAVRN_TARGETED_FRESHNESS_BUSY);
    }
    return targeted_record_receive_status(maintenance, request != 0u ?
        targeted_receive_request(maintenance, input, &origin, sequence, dedupe,
                                  now_ms, action_out) :
        targeted_receive_response(maintenance, input, &origin, sequence, dedupe,
                                  now_ms, action_out));
}

static int targeted_context_route(tavrn_maintenance_t *maintenance,
                                  tavrn_targeted_freshness_context_t *context,
                                  uint32_t now_ms, aodv_route_snapshot_t *route_out)
{
    uint8_t destination;

    if (context->snapshot.work_kind == TAVRN_TARGETED_WORK_LOCAL_REQUEST) {
        tavrn_gtt_entry_t *entry = maintenance_entry(maintenance,
                                                      &context->snapshot.subject);
        tavrn_logical_id_t subject;

        if (entry == NULL || entry->departed != 0u ||
            !logical_subject_for_identity(maintenance, &context->snapshot.subject,
                                          entry, &subject) ||
            tavrn_router_route_to_subject(maintenance->router, &subject, now_ms,
                                          route_out) != TAVRN_ROUTER_ROUTE_OK) {
            return 0;
        }
        return 1;
    }
    destination = context->control.pdu[8];
    return targeted_route_to_sid(maintenance->router, destination, now_ms, route_out);
}

static void targeted_update_receiver(tavrn_targeted_freshness_context_t *context,
                                     const aodv_route_snapshot_t *route)
{
    context->snapshot.receiver = route->next_hop;
    context->control.pdu[7] = (uint8_t)route->next_hop.logical_id.value;
}

static void targeted_expire_context(tavrn_maintenance_t *maintenance,
                                    tavrn_link_v2_t *link, uint8_t index)
{
    tavrn_targeted_freshness_context_t *context =
        targeted_context_from_index(maintenance, index);
    uint8_t canceled = 0u;

    if (context == NULL) {
        return;
    }

    if (context->snapshot.queued != 0u) {
        if (tavrn_link_v2_cancel_queued_tracked_control(
                link, &context->control, context->snapshot.token,
                &canceled) != TAVRN_LINK_RESOLVE_OK || canceled == 0u) {
            context->snapshot.stage = TAVRN_TARGETED_STAGE_CANCELED_TOMBSTONE;
            context->snapshot.queued = 0u;
            context->snapshot.in_flight = 1u;
            context->retry_pending = 0u;
            return;
        }
        context->snapshot.queued = 0u;
    }
    if (context->snapshot.in_flight != 0u) {
        context->snapshot.stage = TAVRN_TARGETED_STAGE_CANCELED_TOMBSTONE;
        context->snapshot.in_flight = 1u;
        context->retry_pending = 0u;
        return;
    }
    if (context->snapshot.work_kind == TAVRN_TARGETED_WORK_LOCAL_REQUEST) {
        targeted_enter_stage1(context);
        return;
    }
    targeted_retire(maintenance, index);
}

tavrn_targeted_freshness_status_t tavrn_maintenance_targeted_owner_tick(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, tavrn_gtt_t *gtt, uint32_t now_ms,
    tavrn_targeted_freshness_action_t *action_out)
{
    uint8_t index;

    targeted_clear_action(action_out);
    if (!targeted_owners_valid(maintenance, router, link, gtt)) {
        return targeted_record_owner_status(maintenance,
                                            TAVRN_TARGETED_FRESHNESS_INVALID,
                                            action_out);
    }
    for (index = 0u; index < TAVRN_TARGETED_FRESHNESS_CONTEXT_CAPACITY +
         TAVRN_TARGETED_RESPONSE_OBLIGATION_CAPACITY; index++) {
        uint8_t context_index;
        tavrn_targeted_freshness_context_t *context =
            targeted_context_at_ordinal(maintenance, index, &context_index);
        aodv_route_snapshot_t route;
        tavrn_targeted_freshness_status_t status;

        if (context->snapshot.valid == 0u ||
            context->snapshot.stage == TAVRN_TARGETED_STAGE_CANCELED_TOMBSTONE ||
            targeted_is_rreq_stage(context->snapshot.stage)) {
            continue;
        }
        if (context->snapshot.stage == TAVRN_TARGETED_STAGE0_WAIT_RESPONSE) {
            if (time_due(now_ms, context->snapshot.obligation_deadline_ms)) {
                targeted_enter_stage1(context);
            }
            continue;
        }
        if (time_due(now_ms, context->snapshot.obligation_deadline_ms)) {
            targeted_expire_context(maintenance, link, context_index);
            return targeted_record_owner_status(maintenance,
                                                TAVRN_TARGETED_FRESHNESS_OK,
                                                action_out);
        }
        /* A local request is admissible only while its original hard-expiry
         * demand still exists.  FULL passive evidence can refresh GTT without
         * traversing the direct-HELLO cancellation path, so revalidate that
         * complete local eligibility before retaining or retrying queued work. */
        if (context->snapshot.work_kind == TAVRN_TARGETED_WORK_LOCAL_REQUEST &&
            !targeted_local_eligible(maintenance, &context->snapshot.subject,
                                     now_ms, &route)) {
            targeted_expire_context(maintenance, link, context_index);
            return targeted_record_owner_status(maintenance,
                                                TAVRN_TARGETED_FRESHNESS_OK,
                                                action_out);
        }
        if (context->snapshot.work_kind ==
                TAVRN_TARGETED_WORK_INTERMEDIARY_RESPONSE &&
            !time_due(now_ms, context->snapshot.not_before_ms)) {
            continue;
        }
        if (context->snapshot.queued != 0u ||
            (context->snapshot.in_flight != 0u && context->retry_pending == 0u)) {
            continue;
        }
        if ((context->snapshot.work_kind != TAVRN_TARGETED_WORK_LOCAL_REQUEST &&
             !targeted_context_route(maintenance, context, now_ms, &route)) ||
            route.next_hop.logical_id.width != TAVRN_IDENTITY_SID8) {
            if (context->snapshot.work_kind == TAVRN_TARGETED_WORK_LOCAL_REQUEST) {
                targeted_enter_stage1(context);
            } else {
                targeted_retire(maintenance, context_index);
            }
            return targeted_record_owner_status(maintenance,
                                                TAVRN_TARGETED_FRESHNESS_OK,
                                                action_out);
        }
        targeted_update_receiver(context, &route);
        status = targeted_admit(maintenance, context_index, now_ms, action_out);
        return targeted_record_owner_status(maintenance, status, action_out);
    }
    return targeted_record_owner_status(maintenance, TAVRN_TARGETED_FRESHNESS_OK,
                                        action_out);
}

tavrn_targeted_freshness_status_t tavrn_maintenance_targeted_revalidate(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, tavrn_gtt_t *gtt, const tavrn_adva_t *subject,
    uint32_t now_ms)
{
    uint8_t index;

    if (!targeted_owners_valid(maintenance, router, link, gtt) || subject == NULL) {
        return TAVRN_TARGETED_FRESHNESS_INVALID;
    }
    for (index = 0u; index < TAVRN_TARGETED_FRESHNESS_CONTEXT_CAPACITY; index++) {
        tavrn_targeted_freshness_context_t *context = &maintenance->targeted[index];
        aodv_route_snapshot_t route;

        if (context->snapshot.valid == 0u ||
            context->snapshot.work_kind != TAVRN_TARGETED_WORK_LOCAL_REQUEST ||
            !adva_equal(&context->snapshot.subject, subject) ||
            targeted_is_rreq_stage(context->snapshot.stage)) {
            continue;
        }
        if (!targeted_local_eligible(maintenance, subject, now_ms, &route)) {
            targeted_expire_context(maintenance, link, index);
        } else {
            targeted_update_receiver(context, &route);
        }
    }
    return TAVRN_TARGETED_FRESHNESS_OK;
}

tavrn_targeted_freshness_status_t tavrn_maintenance_targeted_terminal(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, const ble_mesh_sched_event_t *event,
    uint32_t now_ms)
{
    uint8_t index;

    if (!targeted_owners_valid(maintenance, router, link, maintenance->gtt) ||
        event == NULL ||
        (event->type != BLE_MESH_SCHED_EVENT_TX_DONE &&
          event->type != BLE_MESH_SCHED_EVENT_TX_FAILED)) {
        return TAVRN_TARGETED_FRESHNESS_INVALID;
    }
    if (event->tx_token < 0x8000u) {
        return TAVRN_TARGETED_FRESHNESS_OK;
    }
    for (index = 0u; index < TAVRN_TARGETED_FRESHNESS_CONTEXT_CAPACITY +
         TAVRN_TARGETED_RESPONSE_OBLIGATION_CAPACITY; index++) {
        uint8_t context_index;
        tavrn_targeted_freshness_context_t *context =
            targeted_context_at_ordinal(maintenance, index, &context_index);
        uint8_t canceled;

        if (context->snapshot.valid == 0u || context->snapshot.token != event->tx_token) {
            continue;
        }
        if (targeted_is_rreq_stage(context->snapshot.stage)) {
            return TAVRN_TARGETED_FRESHNESS_OK;
        }
        if (context->snapshot.stage == TAVRN_TARGETED_STAGE_CANCELED_TOMBSTONE) {
            targeted_retire(maintenance, context_index);
            return TAVRN_TARGETED_FRESHNESS_OK;
        }
        canceled = 0u;
        (void)tavrn_link_v2_cancel_queued_tracked_control(
            link, &context->control, context->snapshot.token, &canceled);
        context->snapshot.queued = 0u;
        if (event->type == BLE_MESH_SCHED_EVENT_TX_DONE &&
            event->tx_completed_channel_mask != 0u) {
            context->snapshot.in_flight = 0u;
            context->retry_pending = 0u;
            if (context->snapshot.work_kind == TAVRN_TARGETED_WORK_LOCAL_REQUEST) {
                context->snapshot.stage = TAVRN_TARGETED_STAGE0_WAIT_RESPONSE;
                context->snapshot.obligation_deadline_ms = now_ms +
                    maintenance->config.aodv_net_traversal_ms;
                context->snapshot.token = BLE_MESH_TX_TOKEN_NONE;
            } else {
                targeted_retire(maintenance, context_index);
            }
            return TAVRN_TARGETED_FRESHNESS_OK;
        }
        /* A zero-channel TX_FAILED is no physical attempt.  It keeps exact
         * work retriable, while the in-flight marker lets same-subject liveness
         * retain a tombstone if completion and cancellation race. */
        context->snapshot.in_flight = 1u;
        context->retry_pending = 1u;
        return TAVRN_TARGETED_FRESHNESS_OK;
    }
    return TAVRN_TARGETED_FRESHNESS_OK;
}

tavrn_targeted_freshness_status_t tavrn_maintenance_targeted_scheduler_event(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, const ble_mesh_sched_event_t *event,
    uint32_t now_ms)
{
    uint8_t index;

    if (!targeted_owners_valid(maintenance, router, link, maintenance->gtt) ||
        event == NULL) {
        return targeted_record_scheduler_status(maintenance,
                                                TAVRN_TARGETED_FRESHNESS_INVALID);
    }
    if (event->type == BLE_MESH_SCHED_EVENT_TX_DONE ||
        event->type == BLE_MESH_SCHED_EVENT_TX_FAILED) {
        return targeted_record_scheduler_status(
            maintenance, tavrn_maintenance_targeted_terminal(maintenance, router,
                                                               link, event, now_ms));
    }
    if (event->type != BLE_MESH_SCHED_EVENT_RADIO_FAULT &&
        event->type != BLE_MESH_SCHED_EVENT_SERVICE_FAULT) {
        return targeted_record_scheduler_status(maintenance,
                                                TAVRN_TARGETED_FRESHNESS_OK);
    }
    for (index = 0u; index < TAVRN_TARGETED_FRESHNESS_CONTEXT_CAPACITY +
         TAVRN_TARGETED_RESPONSE_OBLIGATION_CAPACITY; index++) {
        uint8_t context_index;
        tavrn_targeted_freshness_context_t *context =
            targeted_context_at_ordinal(maintenance, index, &context_index);

        if (context->snapshot.valid == 0u ||
            targeted_is_rreq_stage(context->snapshot.stage)) {
            continue;
        }
        if (context->snapshot.stage == TAVRN_TARGETED_STAGE_CANCELED_TOMBSTONE) {
            targeted_retire(maintenance, context_index);
            continue;
        }
        targeted_expire_context(maintenance, link, context_index);
        context = targeted_context_from_index(maintenance, context_index);
        if (context != NULL && context->snapshot.valid != 0u &&
            context->snapshot.stage == TAVRN_TARGETED_STAGE_CANCELED_TOMBSTONE) {
            if (context->snapshot.work_kind == TAVRN_TARGETED_WORK_LOCAL_REQUEST) {
                targeted_enter_stage1(context);
            } else {
                targeted_retire(maintenance, context_index);
            }
        }
    }
    return targeted_record_scheduler_status(maintenance, TAVRN_TARGETED_FRESHNESS_OK);
}

tavrn_targeted_freshness_status_t tavrn_maintenance_targeted_cancel_subject(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, tavrn_gtt_t *gtt, const tavrn_adva_t *subject,
    uint32_t now_ms)
{
    uint8_t index;

    (void)now_ms;
    if (!targeted_owners_valid(maintenance, router, link, gtt) || subject == NULL) {
        return TAVRN_TARGETED_FRESHNESS_INVALID;
    }
    for (index = 0u; index < TAVRN_TARGETED_FRESHNESS_CONTEXT_CAPACITY +
         TAVRN_TARGETED_RESPONSE_OBLIGATION_CAPACITY; index++) {
        uint8_t context_index;
        tavrn_targeted_freshness_context_t *context =
            targeted_context_at_ordinal(maintenance, index, &context_index);
        uint8_t canceled = 0u;

        if (context->snapshot.valid == 0u ||
            !adva_equal(&context->snapshot.subject, subject)) {
            continue;
        }
        if (context->snapshot.in_flight != 0u) {
            context->snapshot.stage = TAVRN_TARGETED_STAGE_CANCELED_TOMBSTONE;
            context->snapshot.queued = 0u;
            continue;
        }
        if (context->snapshot.queued != 0u) {
            if (tavrn_link_v2_cancel_queued_tracked_control(
                    link, &context->control, context->snapshot.token,
                    &canceled) != TAVRN_LINK_RESOLVE_OK) {
                return TAVRN_TARGETED_FRESHNESS_INVALID;
            }
            if (canceled == 0u) {
                context->snapshot.stage = TAVRN_TARGETED_STAGE_CANCELED_TOMBSTONE;
                context->snapshot.queued = 0u;
                continue;
            }
        }
        targeted_retire(maintenance, context_index);
    }
    return TAVRN_TARGETED_FRESHNESS_OK;
}

tavrn_targeted_freshness_status_t tavrn_maintenance_targeted_evicted(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, uint16_t token, uint32_t now_ms)
{
    uint8_t index;

    (void)now_ms;
    if (!targeted_owners_valid(maintenance, router, link, maintenance->gtt)) {
        return TAVRN_TARGETED_FRESHNESS_INVALID;
    }
    if (token < 0x8000u) {
        return TAVRN_TARGETED_FRESHNESS_OK;
    }
    for (index = 0u; index < TAVRN_TARGETED_FRESHNESS_CONTEXT_CAPACITY +
         TAVRN_TARGETED_RESPONSE_OBLIGATION_CAPACITY; index++) {
        uint8_t context_index;
        tavrn_targeted_freshness_context_t *context =
            targeted_context_at_ordinal(maintenance, index, &context_index);

        if (context->snapshot.valid == 0u || context->snapshot.token != token) {
            continue;
        }
        if (context->snapshot.stage == TAVRN_TARGETED_STAGE_CANCELED_TOMBSTONE) {
            targeted_retire(maintenance, context_index);
        } else {
            context->snapshot.queued = 0u;
            context->snapshot.in_flight = 0u;
            context->retry_pending = 1u;
        }
        break;
    }
    return TAVRN_TARGETED_FRESHNESS_OK;
}

#if defined(BLE_RADIO_HOST_TEST)
tavrn_targeted_freshness_status_t tavrn_maintenance_targeted_seed_high_token(
    tavrn_maintenance_t *maintenance, uint16_t next_token)
{
    if (!maintenance_is_initialized(maintenance) || next_token < 0x8000u) {
        return TAVRN_TARGETED_FRESHNESS_INVALID;
    }
    maintenance->targeted_next_token = next_token;
    return TAVRN_TARGETED_FRESHNESS_OK;
}
#endif

tavrn_targeted_freshness_status_t tavrn_maintenance_targeted_snapshot(
    const tavrn_maintenance_t *maintenance, const tavrn_router_t *router,
    const tavrn_link_v2_t *link, const tavrn_gtt_t *gtt,
    tavrn_targeted_freshness_snapshot_t *snapshot_out)
{
    uint8_t index;

    if (snapshot_out != NULL) {
        memset(snapshot_out, 0, sizeof(*snapshot_out));
    }
    if (!targeted_owners_valid(maintenance, router, link, gtt) || snapshot_out == NULL) {
        return TAVRN_TARGETED_FRESHNESS_INVALID;
    }
    for (index = 0u; index < TAVRN_TARGETED_FRESHNESS_CONTEXT_CAPACITY; index++) {
        snapshot_out->contexts[index] = maintenance->targeted[index].snapshot;
        if (maintenance->targeted[index].snapshot.valid != 0u) {
            snapshot_out->active_contexts++;
        }
    }
    for (index = 0u; index < TAVRN_TARGETED_RESPONSE_OBLIGATION_CAPACITY; index++) {
        snapshot_out->response_contexts[index] =
            maintenance->targeted_response[index].snapshot;
        if (maintenance->targeted_response[index].snapshot.valid != 0u) {
            snapshot_out->active_response_contexts++;
        }
    }
    for (index = 0u; index < TAVRN_MAINTENANCE_DEDUPE_CAPACITY; index++) {
        if (maintenance->dedupe[index].valid != 0u) {
            snapshot_out->dedupe_live++;
        }
    }
    snapshot_out->next_node_sequence = maintenance->snapshot.next_node_sequence;
    snapshot_out->reservation_frontier = maintenance->targeted_reservation_frontier;
    snapshot_out->ordinary_pending = maintenance->snapshot.pending;
    return TAVRN_TARGETED_FRESHNESS_OK;
}

/* The retained Stage-0 local-request slots become the sole Stage-1/Stage-2
 * contexts below.  No AODV discovery or pending-DATA state is created here. */
static int verification_owners_valid(const tavrn_maintenance_t *maintenance,
                                     const tavrn_router_t *router,
                                     const tavrn_link_v2_t *link,
                                     const tavrn_gtt_t *gtt)
{
    return targeted_owners_valid(maintenance, router, link, gtt) &&
        router->aodv != NULL && router->aodv->config.path_discovery_ms != 0u &&
        router->aodv->config.path_discovery_ms < TAVRN_MAINTENANCE_HALF_RANGE &&
        router->aodv->config.net_diameter != 0u &&
        router->aodv->config.net_diameter <= 15u;
}

static void verification_clear_action(tavrn_rreq_verification_action_t *action_out)
{
    if (action_out != NULL) {
        memset(action_out, 0, sizeof(*action_out));
    }
}

static tavrn_rreq_verification_stage_t verification_stage(
    tavrn_targeted_freshness_stage_t stage)
{
    switch (stage) {
    case TAVRN_TARGETED_STAGE1_READY:
        return TAVRN_RREQ_VERIFICATION_STAGE1_READY;
    case TAVRN_TARGETED_STAGE1_WAIT_RESPONSE:
        return TAVRN_RREQ_VERIFICATION_STAGE1_WAIT_RESPONSE;
    case TAVRN_TARGETED_STAGE2_READY:
        return TAVRN_RREQ_VERIFICATION_STAGE2_READY;
    case TAVRN_TARGETED_STAGE2_WAIT_RESPONSE:
        return TAVRN_RREQ_VERIFICATION_STAGE2_WAIT_RESPONSE;
    case TAVRN_TARGETED_STAGE_WAIT_DIRECT_DEADLINE:
        return TAVRN_RREQ_VERIFICATION_WAIT_DIRECT_DEADLINE;
    case TAVRN_TARGETED_STAGE_DEPARTED:
        return TAVRN_RREQ_VERIFICATION_DEPARTED;
    case TAVRN_TARGETED_STAGE_DEPARTURE_DEFERRED:
        return TAVRN_RREQ_VERIFICATION_DEPARTURE_DEFERRED;
    default:
        return TAVRN_RREQ_VERIFICATION_STAGE_NONE;
    }
}

static int verification_context_is_live(
    const tavrn_targeted_freshness_context_t *context)
{
    return context != NULL && context->snapshot.valid != 0u &&
        context->snapshot.work_kind == TAVRN_TARGETED_WORK_LOCAL_REQUEST &&
        targeted_is_rreq_stage(context->snapshot.stage) &&
        context->snapshot.stage != TAVRN_TARGETED_STAGE_DEPARTED;
}

static aodv_rreq_attempt_t *verification_attempt_for(
    tavrn_targeted_freshness_context_t *context,
    tavrn_rreq_verification_purpose_t purpose)
{
    if (context == NULL) {
        return NULL;
    }
    return purpose == TAVRN_RREQ_VERIFICATION_PURPOSE_STAGE1_RETAINED_HOP ?
        &context->stage1_attempt :
        (purpose == TAVRN_RREQ_VERIFICATION_PURPOSE_STAGE2_FULL_DIAMETER ?
         &context->stage2_attempt : NULL);
}

static const aodv_rreq_attempt_t *verification_const_attempt_for(
    const tavrn_targeted_freshness_context_t *context,
    tavrn_rreq_verification_purpose_t purpose)
{
    return verification_attempt_for((tavrn_targeted_freshness_context_t *)(void *)context,
                                    purpose);
}

static int verification_attempt_equal(const aodv_rreq_attempt_t *left,
                                      const aodv_rreq_attempt_t *right)
{
    return left != NULL && right != NULL &&
        left->discovery_correlation == right->discovery_correlation &&
        left->origin.width == right->origin.width &&
        left->origin.value == right->origin.value &&
        left->destination.width == right->destination.width &&
        left->destination.value == right->destination.value &&
        left->request_id == right->request_id &&
        left->initial_scope == right->initial_scope &&
        left->current_scope == right->current_scope &&
        left->ring_ordinal == right->ring_ordinal;
}

static void verification_fill_action(
    const tavrn_targeted_freshness_context_t *context,
    tavrn_rreq_verification_action_t *action_out)
{
    const aodv_rreq_attempt_t *attempt;

    verification_clear_action(action_out);
    if (context == NULL || action_out == NULL ||
        context->verification_purpose == TAVRN_RREQ_VERIFICATION_PURPOSE_NONE) {
        return;
    }
    attempt = verification_const_attempt_for(context, context->verification_purpose);
    if (attempt == NULL || attempt->request_id == 0u) {
        return;
    }
    action_out->aodv_action.type = AODV_ACTION_SEND_RREQ;
    action_out->aodv_action.detail.control.controlled_flood = 1u;
    action_out->aodv_action.detail.control.control = context->control;
    action_out->aodv_action.detail.control.rreq_attempt = *attempt;
    action_out->aodv_action.detail.control.rreq_attempt_present =
        AODV_RREQ_ATTEMPT_PRESENT;
    action_out->attempt = *attempt;
    action_out->subject = context->snapshot.subject;
    action_out->token = context->snapshot.token;
    action_out->purpose = context->verification_purpose;
    action_out->enqueued = context->snapshot.queued;
}

static int verification_windows(const tavrn_maintenance_t *maintenance,
                                const tavrn_router_t *router,
                                uint32_t *path_out, uint32_t *total_out)
{
    uint64_t total;

    if (maintenance == NULL || router == NULL || router->aodv == NULL ||
        path_out == NULL || total_out == NULL ||
        maintenance->config.aodv_net_traversal_ms == 0u) {
        return 0;
    }
    total = (uint64_t)maintenance->config.aodv_net_traversal_ms +
        2u * (uint64_t)router->aodv->config.path_discovery_ms;
    if (total == 0u || total >= TAVRN_MAINTENANCE_HALF_RANGE) {
        return 0;
    }
    *path_out = router->aodv->config.path_discovery_ms;
    *total_out = (uint32_t)total;
    return 1;
}

static tavrn_rreq_verification_status_t verification_start_attempt(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, tavrn_targeted_freshness_context_t *context,
    tavrn_rreq_verification_purpose_t purpose, uint8_t scope, uint32_t now_ms,
    tavrn_rreq_verification_action_t *action_out)
{
    aodv_rreq_attempt_t *attempt;
    tavrn_logical_id_t destination;
    aodv_action_t action;
    aodv_single_rreq_status_t create_status;
    tavrn_router_tracked_rreq_status_t enqueue_status;
    uint16_t evicted = BLE_MESH_TX_TOKEN_NONE;

    attempt = verification_attempt_for(context, purpose);
    if (attempt == NULL || scope == 0u ||
        scope > router->aodv->config.net_diameter) {
        return TAVRN_RREQ_VERIFICATION_INVALID;
    }
    if (context->snapshot.queued != 0u || context->snapshot.in_flight != 0u) {
        return TAVRN_RREQ_VERIFICATION_IGNORED;
    }
    if (attempt->request_id == 0u) {
        if (!tavrn_link_v2_tracked_control_may_admit(link)) {
            return TAVRN_RREQ_VERIFICATION_BUSY;
        }
        if (!logical_subject_for_identity(maintenance, &context->snapshot.subject,
                                          maintenance_entry(maintenance,
                                                            &context->snapshot.subject),
                                          &destination)) {
            return TAVRN_RREQ_VERIFICATION_UNAVAILABLE;
        }
        memset(&action, 0, sizeof(action));
        create_status = aodv_core_create_single_rreq(router->aodv, &destination,
                                                      scope, now_ms, &action);
        if (create_status == AODV_SINGLE_RREQ_RATE_DEFERRED) {
            maintenance->verification_rreq_rate_deferrals++;
            return TAVRN_RREQ_VERIFICATION_RATE_DEFERRED;
        }
        if (create_status != AODV_SINGLE_RREQ_OK) {
            return create_status == AODV_SINGLE_RREQ_BUSY ?
                TAVRN_RREQ_VERIFICATION_BUSY : TAVRN_RREQ_VERIFICATION_INVALID;
        }
        *attempt = action.detail.control.rreq_attempt;
        context->control = action.detail.control.control;
        context->verification_purpose = purpose;
    }
    if (context->verification_purpose != purpose ||
        !tavrn_link_v2_tracked_control_may_admit(link)) {
        return context->verification_purpose == purpose ?
            TAVRN_RREQ_VERIFICATION_BUSY : TAVRN_RREQ_VERIFICATION_INVALID;
    }
    if (context->snapshot.token == BLE_MESH_TX_TOKEN_NONE) {
        context->snapshot.token = targeted_allocate_high_token(maintenance, link);
        if (context->snapshot.token == BLE_MESH_TX_TOKEN_NONE) {
            return TAVRN_RREQ_VERIFICATION_BUSY;
        }
    }
    verification_fill_action(context, action_out);
    enqueue_status = tavrn_router_enqueue_tracked_rreq(
        router, &action_out->aodv_action, context->snapshot.token, now_ms, &evicted);
    if (evicted >= 0x8000u) {
        tavrn_maintenance_high_token_dispatch_result_t eviction;

        (void)tavrn_maintenance_high_token_evicted(maintenance, router, link,
                                                    evicted, now_ms, &eviction);
    }
    if (enqueue_status == TAVRN_ROUTER_TRACKED_RREQ_OK) {
        context->snapshot.queued = 1u;
        context->snapshot.in_flight = 0u;
        verification_fill_action(context, action_out);
        return TAVRN_RREQ_VERIFICATION_OK;
    }
    verification_clear_action(action_out);
    return enqueue_status == TAVRN_ROUTER_TRACKED_RREQ_BUSY ||
        enqueue_status == TAVRN_ROUTER_TRACKED_RREQ_LOCAL_NOT_ATTEMPTED ?
        TAVRN_RREQ_VERIFICATION_BUSY : TAVRN_RREQ_VERIFICATION_INVALID;
}

static tavrn_rreq_verification_status_t verification_checked_departure(
    tavrn_maintenance_t *maintenance, tavrn_targeted_freshness_context_t *context,
    uint32_t now_ms, uint8_t direct_timeout, uint8_t *departed_out)
{
    tavrn_gtt_entry_t *entry;
    tavrn_gtt_departure_candidate_t candidate;
    tavrn_maintenance_checked_departure_status_t status;

    if (departed_out != NULL) *departed_out = 0u;
    entry = maintenance_entry(maintenance, &context->snapshot.subject);
    if (entry == NULL || entry->departed != 0u) {
        context->snapshot.stage = TAVRN_TARGETED_STAGE_DEPARTURE_DEFERRED;
        return TAVRN_RREQ_VERIFICATION_OK;
    }
    candidate.identity = entry->identity;
    candidate.revision = entry->revision;
    candidate.hard_deadline_ms = entry->hard_deadline_ms;
    status = tavrn_maintenance_checked_local_departure(maintenance, &candidate, now_ms,
                                                        NULL);
    if (status == TAVRN_GTT_CHECKED_DEPARTED) {
        tavrn_tc_metadata_status_t tc_status;

        context->snapshot.stage = TAVRN_TARGETED_STAGE_DEPARTED;
        context->snapshot.queued = 0u;
        context->snapshot.in_flight = 0u;
        context->snapshot.token = BLE_MESH_TX_TOKEN_NONE;
        maintenance->verification_checked_departures++;
        if (direct_timeout == 0u) {
            tc_status = tavrn_maintenance_tc_on_verified_departure(
                maintenance, &candidate.identity, now_ms);
            if (tc_status == TAVRN_TC_METADATA_INVALID) {
                return TAVRN_RREQ_VERIFICATION_INVALID;
            }
        }
        if (departed_out != NULL) *departed_out = 1u;
    } else {
        context->snapshot.stage = TAVRN_TARGETED_STAGE_DEPARTURE_DEFERRED;
    }
    return TAVRN_RREQ_VERIFICATION_OK;
}

tavrn_rreq_verification_status_t tavrn_maintenance_verification_owner_tick(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, tavrn_gtt_t *gtt, uint32_t now_ms,
    tavrn_rreq_verification_action_t *action_out)
{
    uint8_t index;
    uint32_t path_discovery_ms;
    uint32_t verification_window_ms;

    verification_clear_action(action_out);
    if (!verification_owners_valid(maintenance, router, link, gtt) ||
        action_out == NULL || !verification_windows(maintenance, router,
                                                     &path_discovery_ms,
                                                     &verification_window_ms)) {
        return TAVRN_RREQ_VERIFICATION_INVALID;
    }
    for (index = 0u; index < TAVRN_TARGETED_FRESHNESS_CONTEXT_CAPACITY; index++) {
        tavrn_targeted_freshness_context_t *context = &maintenance->targeted[index];
        tavrn_gtt_entry_t *entry;
        tavrn_rreq_verification_status_t status;
        uint16_t scope;

        if (!verification_context_is_live(context)) {
            continue;
        }
        entry = maintenance_entry(maintenance, &context->snapshot.subject);
        if (entry == NULL || entry->departed != 0u) {
            context->snapshot.stage = TAVRN_TARGETED_STAGE_DEPARTURE_DEFERRED;
            continue;
        }
        /* Only a committed same-subject refresh moves hard expiry into the
         * future; revision-only demand changes must not masquerade as evidence. */
        if (entry->revision != context->verification_expected_gtt_revision &&
            !time_due(now_ms, entry->hard_deadline_ms)) {
            tavrn_targeted_freshness_status_t cancel_status =
                tavrn_maintenance_targeted_cancel_subject(
                    maintenance, router, link, gtt, &context->snapshot.subject, now_ms);

            return cancel_status == TAVRN_TARGETED_FRESHNESS_OK ?
                TAVRN_RREQ_VERIFICATION_CANCELED : TAVRN_RREQ_VERIFICATION_INVALID;
        }
        if (context->snapshot.stage == TAVRN_TARGETED_STAGE1_READY) {
            /* Preserve the last hard-expired hop at the first actual RREQ
             * decision.  Stage 0 can reach this handoff before a route exists. */
            if (context->stage1_attempt.request_id == 0u) {
                context->verification_retained_hop = entry->hop_count;
            }
            if (context->verification_retained_hop == 0u) {
                context->snapshot.stage = TAVRN_TARGETED_STAGE2_READY;
            } else {
                scope = (uint16_t)context->verification_retained_hop + 2u;
                if (scope > router->aodv->config.net_diameter) {
                    scope = router->aodv->config.net_diameter;
                }
                status = verification_start_attempt(
                    maintenance, router, link, context,
                    TAVRN_RREQ_VERIFICATION_PURPOSE_STAGE1_RETAINED_HOP,
                    (uint8_t)scope, now_ms, action_out);
                if (status != TAVRN_RREQ_VERIFICATION_IGNORED) {
                    return status;
                }
                continue;
            }
        }
        if (context->snapshot.stage == TAVRN_TARGETED_STAGE1_WAIT_RESPONSE) {
            if (!time_due(now_ms, context->verification_response_deadline_ms)) {
                continue;
            }
            context->snapshot.stage = TAVRN_TARGETED_STAGE2_READY;
        }
        if (context->snapshot.stage == TAVRN_TARGETED_STAGE2_READY) {
            status = verification_start_attempt(
                maintenance, router, link, context,
                TAVRN_RREQ_VERIFICATION_PURPOSE_STAGE2_FULL_DIAMETER,
                router->aodv->config.net_diameter, now_ms, action_out);
            if (status != TAVRN_RREQ_VERIFICATION_IGNORED) {
                return status;
            }
            continue;
        }
        if (context->snapshot.stage == TAVRN_TARGETED_STAGE2_WAIT_RESPONSE) {
            if (!time_due(now_ms, context->verification_response_deadline_ms)) {
                continue;
            }
            if (context->verification_direct_subject != 0u) {
                if (!direct_deadline_for(maintenance, entry,
                                         &context->verification_direct_evidence_deadline_ms)) {
                    context->snapshot.stage = TAVRN_TARGETED_STAGE_DEPARTURE_DEFERRED;
                    return TAVRN_RREQ_VERIFICATION_OK;
                }
                if (!time_due(now_ms, context->verification_direct_evidence_deadline_ms)) {
                    context->snapshot.stage = TAVRN_TARGETED_STAGE_WAIT_DIRECT_DEADLINE;
                    return TAVRN_RREQ_VERIFICATION_OK;
                }
            }
            if (context->verification_direct_subject != 0u) {
                context->snapshot.stage = TAVRN_TARGETED_STAGE_WAIT_DIRECT_DEADLINE;
            }
            {
                tavrn_rreq_verification_status_t departure_status =
                    TAVRN_RREQ_VERIFICATION_OK;
                uint8_t departed = 0u;
                uint8_t direct_timeout = context->verification_direct_subject != 0u;

                departure_status = verification_checked_departure(
                    maintenance, context, now_ms, direct_timeout, &departed);
                if (departure_status == TAVRN_RREQ_VERIFICATION_OK &&
                    direct_timeout != 0u && departed != 0u &&
                    tavrn_maintenance_tc_on_direct_timeout_departure(
                        maintenance, &context->snapshot.subject, now_ms) ==
                        TAVRN_TC_METADATA_INVALID) {
                    return TAVRN_RREQ_VERIFICATION_INVALID;
                }
                return departure_status;
            }
        }
        if (context->snapshot.stage == TAVRN_TARGETED_STAGE_WAIT_DIRECT_DEADLINE) {
            if (!time_due(now_ms, context->verification_direct_evidence_deadline_ms)) {
                continue;
            }
            {
                uint8_t departed = 0u;
                tavrn_rreq_verification_status_t departure_status =
                    verification_checked_departure(
                        maintenance, context, now_ms,
                        verification_stage(context->snapshot.stage) ==
                            TAVRN_RREQ_VERIFICATION_WAIT_DIRECT_DEADLINE,
                        &departed);

                if (departure_status == TAVRN_RREQ_VERIFICATION_OK && departed != 0u &&
                    tavrn_maintenance_tc_on_direct_timeout_departure(
                        maintenance, &context->snapshot.subject, now_ms) ==
                        TAVRN_TC_METADATA_INVALID) {
                    return TAVRN_RREQ_VERIFICATION_INVALID;
                }
                return departure_status;
            }
        }
        if (context->snapshot.stage == TAVRN_TARGETED_STAGE_DEPARTURE_DEFERRED) {
            return verification_checked_departure(maintenance, context, now_ms, 0u, NULL);
        }
    }
    (void)path_discovery_ms;
    (void)verification_window_ms;
    return TAVRN_RREQ_VERIFICATION_OK;
}

tavrn_rreq_verification_status_t tavrn_maintenance_verification_terminal(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link,
    const tavrn_rreq_verification_completion_t *completion, uint32_t now_ms)
{
    uint8_t index;
    uint32_t path_discovery_ms;
    uint32_t verification_window_ms;

    if (!verification_owners_valid(maintenance, router, link, maintenance != NULL ?
                                   maintenance->gtt : NULL) || completion == NULL ||
        completion->token < 0x8000u ||
        (completion->purpose != TAVRN_RREQ_VERIFICATION_PURPOSE_STAGE1_RETAINED_HOP &&
         completion->purpose != TAVRN_RREQ_VERIFICATION_PURPOSE_STAGE2_FULL_DIAMETER) ||
        completion->kind > TAVRN_RREQ_VERIFICATION_TERMINAL_LOCAL_NOT_ATTEMPTED ||
        !verification_windows(maintenance, router, &path_discovery_ms,
                              &verification_window_ms)) {
        return TAVRN_RREQ_VERIFICATION_INVALID;
    }
    for (index = 0u; index < TAVRN_TARGETED_FRESHNESS_CONTEXT_CAPACITY; index++) {
        tavrn_targeted_freshness_context_t *context = &maintenance->targeted[index];
        const aodv_rreq_attempt_t *attempt;
        uint8_t canceled = 0u;

        if (context->snapshot.valid == 0u ||
            context->snapshot.token != completion->token) {
            continue;
        }
        if (context->verification_purpose != completion->purpose) {
            return TAVRN_RREQ_VERIFICATION_IGNORED;
        }
        attempt = verification_const_attempt_for(context, completion->purpose);
        if (!verification_attempt_equal(attempt, &completion->attempt)) {
            return TAVRN_RREQ_VERIFICATION_IGNORED;
        }
        if (context->snapshot.stage == TAVRN_TARGETED_STAGE_CANCELED_TOMBSTONE) {
            targeted_retire(maintenance, index);
            return TAVRN_RREQ_VERIFICATION_OK;
        }
        if (context->snapshot.queued != 0u &&
            tavrn_link_v2_cancel_queued_tracked_control(
                link, &context->control, context->snapshot.token, &canceled) !=
                TAVRN_LINK_RESOLVE_OK) {
            return TAVRN_RREQ_VERIFICATION_INVALID;
        }
        context->snapshot.queued = 0u;
        context->snapshot.in_flight = 0u;
        if (completion->kind == TAVRN_RREQ_VERIFICATION_TERMINAL_TX_DONE &&
            completion->completed_channel_mask != 0u) {
            if (context->verification_deadline_ms == 0u) {
                context->verification_deadline_ms = now_ms + verification_window_ms;
            }
            context->verification_response_deadline_ms = now_ms + path_discovery_ms;
            context->snapshot.token = BLE_MESH_TX_TOKEN_NONE;
            if (completion->purpose ==
                TAVRN_RREQ_VERIFICATION_PURPOSE_STAGE1_RETAINED_HOP) {
                context->snapshot.stage = TAVRN_TARGETED_STAGE1_WAIT_RESPONSE;
            } else {
                context->snapshot.stage = TAVRN_TARGETED_STAGE2_WAIT_RESPONSE;
            }
        }
        return TAVRN_RREQ_VERIFICATION_OK;
    }
    return TAVRN_RREQ_VERIFICATION_IGNORED;
}

tavrn_rreq_verification_status_t tavrn_maintenance_verification_receive_rrep(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, tavrn_gtt_t *gtt,
    const tavrn_rx_control_event_t *control_event, uint32_t now_ms)
{
    aodv_control_input_t input;
    tavrn_targeted_freshness_context_t *matched = NULL;
    uint8_t index;
    uint8_t matches = 0u;
    aodv_rrep_attempt_match_status_t match_status;
    aodv_status_t ingest_status;
    aodv_route_snapshot_t route;
    tavrn_logical_id_t destination;
    tavrn_targeted_freshness_status_t cancel_status;
    const aodv_rreq_attempt_t *matched_attempt;

    if (!verification_owners_valid(maintenance, router, link, gtt) ||
        control_event == NULL) {
        return TAVRN_RREQ_VERIFICATION_INVALID;
    }
    if (control_event->control.type != TAVRN_WIRE_E_RREP) {
        return TAVRN_RREQ_VERIFICATION_IGNORED;
    }
    memset(&input, 0, sizeof(input));
    input.transmitter = control_event->transmitter;
    input.control = control_event->control;
    for (index = 0u; index < TAVRN_TARGETED_FRESHNESS_CONTEXT_CAPACITY; index++) {
        tavrn_targeted_freshness_context_t *context = &maintenance->targeted[index];
        const aodv_rreq_attempt_t *attempt;

        if (!verification_context_is_live(context) ||
            (context->snapshot.stage != TAVRN_TARGETED_STAGE1_WAIT_RESPONSE &&
             context->snapshot.stage != TAVRN_TARGETED_STAGE2_WAIT_RESPONSE)) {
            continue;
        }
        attempt = verification_const_attempt_for(context, context->verification_purpose);
        if (attempt == NULL) {
            return TAVRN_RREQ_VERIFICATION_INVALID;
        }
        match_status = aodv_core_rrep_matches_attempt(router->aodv, &input, attempt);
        if (match_status == AODV_RREP_ATTEMPT_INVALID) {
            return TAVRN_RREQ_VERIFICATION_INVALID;
        }
        if (match_status == AODV_RREP_ATTEMPT_MATCH) {
            matched = context;
            matches++;
        }
    }
    if (matches != 1u || matched == NULL) {
        return TAVRN_RREQ_VERIFICATION_IGNORED;
    }
    matched_attempt = verification_const_attempt_for(matched,
                                                      matched->verification_purpose);
    if (matched_attempt == NULL ||
        !logical_subject_for_identity(maintenance, &matched->snapshot.subject,
                                      maintenance_entry(maintenance,
                                                        &matched->snapshot.subject),
                                      &destination) ||
        destination.width != matched_attempt->destination.width ||
        destination.value != matched_attempt->destination.value) {
        return TAVRN_RREQ_VERIFICATION_IGNORED;
    }
    ingest_status = tavrn_router_ingest_rrep_for_attempt(
        router, control_event, matched_attempt, now_ms);
    if (ingest_status != AODV_STATUS_OK && ingest_status != AODV_STATUS_DUPLICATE) {
        return ingest_status == AODV_STATUS_BUSY ? TAVRN_RREQ_VERIFICATION_BUSY :
                                                   TAVRN_RREQ_VERIFICATION_IGNORED;
    }
    if (tavrn_router_route_to_subject(router, &destination, now_ms, &route) !=
            TAVRN_ROUTER_ROUTE_OK ||
        route.destination.width != destination.width ||
        route.destination.value != destination.value) {
        return TAVRN_RREQ_VERIFICATION_INVALID;
    }
    cancel_status = tavrn_maintenance_targeted_cancel_subject(
        maintenance, router, link, gtt, &matched->snapshot.subject, now_ms);
    return cancel_status == TAVRN_TARGETED_FRESHNESS_OK ?
        TAVRN_RREQ_VERIFICATION_CANCELED : TAVRN_RREQ_VERIFICATION_INVALID;
}

tavrn_rreq_verification_status_t tavrn_maintenance_verification_scheduler_event(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, const ble_mesh_sched_event_t *event,
    uint32_t now_ms)
{
    uint8_t index;

    if (!verification_owners_valid(maintenance, router, link,
                                   maintenance != NULL ? maintenance->gtt : NULL) ||
        event == NULL) {
        return TAVRN_RREQ_VERIFICATION_INVALID;
    }
    if (event->type == BLE_MESH_SCHED_EVENT_TX_DONE ||
        event->type == BLE_MESH_SCHED_EVENT_TX_FAILED) {
        tavrn_rreq_verification_completion_t completion;

        if (event->tx_token < 0x8000u) {
            return TAVRN_RREQ_VERIFICATION_IGNORED;
        }
        for (index = 0u; index < TAVRN_TARGETED_FRESHNESS_CONTEXT_CAPACITY; index++) {
            const tavrn_targeted_freshness_context_t *context =
                &maintenance->targeted[index];
            const aodv_rreq_attempt_t *attempt;

            if (context->snapshot.valid == 0u ||
                context->snapshot.token != event->tx_token ||
                context->verification_purpose ==
                    TAVRN_RREQ_VERIFICATION_PURPOSE_NONE) {
                continue;
            }
            attempt = verification_const_attempt_for(context,
                                                     context->verification_purpose);
            if (attempt == NULL || attempt->request_id == 0u) {
                return TAVRN_RREQ_VERIFICATION_INVALID;
            }
            memset(&completion, 0, sizeof(completion));
            completion.attempt = *attempt;
            completion.token = event->tx_token;
            completion.purpose = context->verification_purpose;
            completion.kind = event->type == BLE_MESH_SCHED_EVENT_TX_DONE ?
                TAVRN_RREQ_VERIFICATION_TERMINAL_TX_DONE :
                TAVRN_RREQ_VERIFICATION_TERMINAL_TX_FAILED;
            completion.completed_channel_mask = event->tx_completed_channel_mask;
            return tavrn_maintenance_verification_terminal(maintenance, router, link,
                                                            &completion, now_ms);
        }
        return TAVRN_RREQ_VERIFICATION_IGNORED;
    }
    if (event->type != BLE_MESH_SCHED_EVENT_RADIO_FAULT &&
        event->type != BLE_MESH_SCHED_EVENT_SERVICE_FAULT) {
        return TAVRN_RREQ_VERIFICATION_IGNORED;
    }
    for (index = 0u; index < TAVRN_TARGETED_FRESHNESS_CONTEXT_CAPACITY; index++) {
        tavrn_targeted_freshness_context_t *context = &maintenance->targeted[index];
        uint8_t canceled = 0u;

        if (!verification_context_is_live(context)) {
            continue;
        }
        if (context->snapshot.queued != 0u) {
            if (tavrn_link_v2_cancel_queued_tracked_control(
                    link, &context->control, context->snapshot.token, &canceled) !=
                TAVRN_LINK_RESOLVE_OK) {
                return TAVRN_RREQ_VERIFICATION_INVALID;
            }
        }
        context->snapshot.queued = 0u;
        context->snapshot.in_flight = 0u;
    }
    return TAVRN_RREQ_VERIFICATION_OK;
}

static void high_token_dispatch_clear(
    tavrn_maintenance_high_token_dispatch_result_t *result)
{
    memset(result, 0, sizeof(*result));
    result->targeted_status = TAVRN_TARGETED_FRESHNESS_OK;
    result->verification_status = TAVRN_RREQ_VERIFICATION_IGNORED;
    result->external_status = TAVRN_MAINTENANCE_HIGH_TOKEN_NOT_FOUND;
}

static tavrn_maintenance_high_token_status_t high_token_dispatch_status(
    const tavrn_maintenance_high_token_dispatch_result_t *result)
{
    if (result->targeted_status == TAVRN_TARGETED_FRESHNESS_INVALID ||
        result->verification_status == TAVRN_RREQ_VERIFICATION_INVALID) {
        return TAVRN_MAINTENANCE_HIGH_TOKEN_INVALID;
    }
    if (result->external_handled != 0u) {
        return result->external_status;
    }
    return TAVRN_MAINTENANCE_HIGH_TOKEN_OK;
}

static void high_token_dispatch_external(
    const tavrn_maintenance_t *maintenance,
    const ble_mesh_sched_event_t *event,
    tavrn_maintenance_high_token_completion_kind_t kind,
    uint32_t now_ms,
    tavrn_maintenance_high_token_dispatch_result_t *result)
{
    tavrn_maintenance_external_high_token_registration_t registration;
    tavrn_maintenance_high_token_completion_t completion;

    if (maintenance == NULL || result == NULL ||
        maintenance->external_high_token.valid == 0u) {
        return;
    }
    registration = maintenance->external_high_token;
    result->external_handled = 1u;
    if (registration.completion == NULL) {
        result->external_status = TAVRN_MAINTENANCE_HIGH_TOKEN_INVALID;
        return;
    }
    memset(&completion, 0, sizeof(completion));
    completion.owner = registration.owner;
    completion.purpose = registration.purpose;
    completion.token = registration.token;
    completion.kind = kind;
    completion.completed_at_ms = now_ms;
    if (event != NULL) {
        completion.requested_channel_mask = event->tx_requested_channel_mask;
        completion.completed_channel_mask = event->tx_completed_channel_mask;
    }
    result->external_status = registration.completion(registration.context,
                                                       &completion);
}

static void high_token_dispatch_external_fault(
    tavrn_maintenance_t *maintenance, tavrn_link_v2_t *link,
    const ble_mesh_sched_event_t *event,
    tavrn_maintenance_high_token_completion_kind_t kind, uint32_t now_ms,
    tavrn_maintenance_high_token_dispatch_result_t *result)
{
    tavrn_maintenance_external_high_token_registration_t registration;
    tavrn_maintenance_high_token_completion_t completion;
    uint8_t canceled = 0u;

    if (maintenance == NULL || link == NULL || result == NULL ||
        maintenance->external_high_token.valid == 0u) {
        return;
    }
    registration = maintenance->external_high_token;
    result->external_handled = 1u;
    if (registration.completion == NULL ||
        tavrn_link_v2_cancel_queued_tracked_token(
            link, registration.token, &canceled) != TAVRN_LINK_RESOLVE_OK ||
        tavrn_link_v2_tracked_token_in_use(link, registration.token)) {
        result->external_status = TAVRN_MAINTENANCE_HIGH_TOKEN_INVALID;
        return;
    }
    /* The live registration must remain untouched across this copied callback:
     * repair releases the exact tuple from inside its fault completion. */
    memset(&completion, 0, sizeof(completion));
    completion.owner = registration.owner;
    completion.purpose = registration.purpose;
    completion.token = registration.token;
    completion.kind = kind;
    completion.completed_at_ms = now_ms;
    if (event != NULL) {
        completion.requested_channel_mask = event->tx_requested_channel_mask;
        completion.completed_channel_mask = event->tx_completed_channel_mask;
    }
    result->external_status = registration.completion(registration.context,
                                                       &completion);
}

static tavrn_rreq_verification_status_t high_token_verification_evicted(
    tavrn_maintenance_t *maintenance, uint16_t token)
{
    uint8_t index;

    for (index = 0u; index < TAVRN_TARGETED_FRESHNESS_CONTEXT_CAPACITY; index++) {
        tavrn_targeted_freshness_context_t *context = &maintenance->targeted[index];

        if (context->snapshot.valid == 0u || context->snapshot.token != token ||
            !targeted_is_rreq_stage(context->snapshot.stage)) {
            continue;
        }
        if (context->snapshot.stage == TAVRN_TARGETED_STAGE_CANCELED_TOMBSTONE) {
            targeted_retire(maintenance, index);
        } else {
            context->snapshot.queued = 0u;
            context->snapshot.in_flight = 0u;
        }
        return TAVRN_RREQ_VERIFICATION_OK;
    }
    return TAVRN_RREQ_VERIFICATION_IGNORED;
}

tavrn_maintenance_high_token_status_t
tavrn_maintenance_high_token_scheduler_event(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, const ble_mesh_sched_event_t *event,
    uint32_t now_ms, tavrn_maintenance_high_token_dispatch_result_t *result_out)
{
    if (result_out == NULL) {
        return TAVRN_MAINTENANCE_HIGH_TOKEN_INVALID;
    }
    high_token_dispatch_clear(result_out);
    if (maintenance == NULL || router == NULL || event == NULL ||
        !targeted_owners_valid(maintenance, router, link, maintenance->gtt)) {
        result_out->targeted_status = TAVRN_TARGETED_FRESHNESS_INVALID;
        result_out->verification_status = TAVRN_RREQ_VERIFICATION_INVALID;
        result_out->external_status = TAVRN_MAINTENANCE_HIGH_TOKEN_INVALID;
        return TAVRN_MAINTENANCE_HIGH_TOKEN_INVALID;
    }
    if (event->type == BLE_MESH_SCHED_EVENT_TX_DONE ||
        event->type == BLE_MESH_SCHED_EVENT_TX_FAILED) {
        tavrn_maintenance_high_token_completion_kind_t kind =
            event->type == BLE_MESH_SCHED_EVENT_TX_DONE ?
                TAVRN_MAINTENANCE_HIGH_TOKEN_COMPLETION_TX_DONE :
                TAVRN_MAINTENANCE_HIGH_TOKEN_COMPLETION_TX_FAILED;

        if (event->tx_token < 0x8000u) {
            return TAVRN_MAINTENANCE_HIGH_TOKEN_OK;
        }
        if (high_token_owned_by_external(maintenance, event->tx_token)) {
            high_token_dispatch_external(maintenance, event, kind, now_ms, result_out);
            return high_token_dispatch_status(result_out);
        }
        result_out->targeted_handled =
            (uint8_t)high_token_owned_by_targeted(maintenance, event->tx_token);
        result_out->verification_handled =
            (uint8_t)high_token_owned_by_verification(maintenance, event->tx_token);
        result_out->targeted_status = tavrn_maintenance_targeted_scheduler_event(
            maintenance, router, link, event, now_ms);
        result_out->verification_status =
            tavrn_maintenance_verification_scheduler_event(
                maintenance, router, link, event, now_ms);
        return high_token_dispatch_status(result_out);
    }
    if (event->type == BLE_MESH_SCHED_EVENT_RADIO_FAULT ||
        event->type == BLE_MESH_SCHED_EVENT_SERVICE_FAULT) {
        tavrn_maintenance_high_token_completion_kind_t kind =
            event->type == BLE_MESH_SCHED_EVENT_RADIO_FAULT ?
                TAVRN_MAINTENANCE_HIGH_TOKEN_COMPLETION_RADIO_FAULT :
                TAVRN_MAINTENANCE_HIGH_TOKEN_COMPLETION_SERVICE_FAULT;

        result_out->targeted_handled =
            (uint8_t)high_token_targeted_owner_active(maintenance);
        result_out->verification_handled =
            (uint8_t)high_token_verification_owner_active(maintenance);
        result_out->targeted_status = tavrn_maintenance_targeted_scheduler_event(
            maintenance, router, link, event, now_ms);
        result_out->verification_status =
            tavrn_maintenance_verification_scheduler_event(
                maintenance, router, link, event, now_ms);
        high_token_dispatch_external_fault(maintenance, link, event, kind, now_ms,
                                            result_out);
        return high_token_dispatch_status(result_out);
    }
    return TAVRN_MAINTENANCE_HIGH_TOKEN_OK;
}

tavrn_maintenance_high_token_status_t tavrn_maintenance_high_token_evicted(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, uint16_t token, uint32_t now_ms,
    tavrn_maintenance_high_token_dispatch_result_t *result_out)
{
    if (result_out == NULL) {
        return TAVRN_MAINTENANCE_HIGH_TOKEN_INVALID;
    }
    high_token_dispatch_clear(result_out);
    if (maintenance == NULL || router == NULL || token < 0x8000u ||
        !targeted_owners_valid(maintenance, router, link, maintenance->gtt)) {
        result_out->targeted_status = TAVRN_TARGETED_FRESHNESS_INVALID;
        result_out->verification_status = TAVRN_RREQ_VERIFICATION_INVALID;
        result_out->external_status = TAVRN_MAINTENANCE_HIGH_TOKEN_INVALID;
        return TAVRN_MAINTENANCE_HIGH_TOKEN_INVALID;
    }
    if (high_token_owned_by_external(maintenance, token)) {
        high_token_dispatch_external(
            maintenance, NULL, TAVRN_MAINTENANCE_HIGH_TOKEN_COMPLETION_TX_FAILED,
            now_ms, result_out);
        return high_token_dispatch_status(result_out);
    }
    if (high_token_owned_by_verification(maintenance, token)) {
        result_out->verification_handled = 1u;
        result_out->verification_status = high_token_verification_evicted(maintenance,
                                                                            token);
        return high_token_dispatch_status(result_out);
    }
    if (high_token_owned_by_targeted(maintenance, token)) {
        result_out->targeted_handled = 1u;
        result_out->targeted_status = tavrn_maintenance_targeted_evicted(
            maintenance, router, link, token, now_ms);
    }
    return high_token_dispatch_status(result_out);
}

tavrn_rreq_verification_status_t tavrn_maintenance_verification_snapshot(
    const tavrn_maintenance_t *maintenance, const tavrn_router_t *router,
    const tavrn_link_v2_t *link, const tavrn_gtt_t *gtt,
    tavrn_rreq_verification_snapshot_t *snapshot_out)
{
    uint8_t index;

    if (snapshot_out != NULL) {
        memset(snapshot_out, 0, sizeof(*snapshot_out));
    }
    if (!verification_owners_valid(maintenance, router, link, gtt) ||
        snapshot_out == NULL) {
        return TAVRN_RREQ_VERIFICATION_INVALID;
    }
    for (index = 0u; index < TAVRN_TARGETED_FRESHNESS_CONTEXT_CAPACITY; index++) {
        const tavrn_targeted_freshness_context_t *context = &maintenance->targeted[index];
        tavrn_rreq_verification_context_snapshot_t *copy =
            &snapshot_out->contexts[index];

        if (context->snapshot.valid == 0u ||
            context->snapshot.work_kind != TAVRN_TARGETED_WORK_LOCAL_REQUEST) {
            continue;
        }
        copy->subject = context->snapshot.subject;
        copy->expected_gtt_revision = context->verification_expected_gtt_revision;
        copy->response_deadline_ms = context->verification_response_deadline_ms;
        copy->direct_evidence_deadline_ms =
            context->verification_direct_evidence_deadline_ms;
        copy->verification_deadline_ms = context->verification_deadline_ms;
        copy->stage1_request_id = context->stage1_attempt.request_id;
        copy->stage2_request_id = context->stage2_attempt.request_id;
        copy->token = context->snapshot.token;
        copy->retained_hop = context->verification_retained_hop;
        copy->direct_subject = context->verification_direct_subject;
        copy->valid = context->snapshot.valid;
        copy->stage = verification_stage(context->snapshot.stage);
        if (verification_context_is_live(context)) {
            snapshot_out->active_contexts++;
        }
    }
    snapshot_out->rreq_rate_deferrals = maintenance->verification_rreq_rate_deferrals;
    snapshot_out->checked_departures = maintenance->verification_checked_departures;
    return TAVRN_RREQ_VERIFICATION_OK;
}

/* ------------------------------------------------------------------------- */
/* DEV-027 TC and general metadata.  This is intentionally below the targeted
 * verifier: both use the same maintenance owner, but TC/metadata never leaks
 * a FULL type into AODV or link-v2. */

static int tc_metadata_adva_valid(const tavrn_adva_t *identity)
{
    uint8_t random_all_zero = 1u;
    uint8_t random_all_one = 1u;
    uint8_t index;

    if (identity == NULL || (identity->bytes[5] & 0xc0u) != 0xc0u) {
        return 0;
    }
    for (index = 0u; index < TAVRN_ADVA_LEN; index++) {
        if (identity->bytes[index] != 0u) {
            random_all_zero = 0u;
        }
        if (identity->bytes[index] != 0xffu) {
            random_all_one = 0u;
        }
    }
    return random_all_zero == 0u && random_all_one == 0u;
}

static int tc_metadata_state_valid(const tavrn_tc_metadata_state_t *state)
{
    return state != NULL && state->initialized != 0u &&
        tc_metadata_adva_valid(&state->config.local_identity) &&
        state->config.tc_uuid_ms != 0u &&
        state->config.tc_uuid_ms < TAVRN_MAINTENANCE_HALF_RANGE &&
        state->config.tc_subject_ms != 0u &&
        state->config.tc_subject_ms < TAVRN_MAINTENANCE_HALF_RANGE &&
        state->config.metadata_cooldown_ms != 0u &&
        state->config.metadata_cooldown_ms < TAVRN_MAINTENANCE_HALF_RANGE;
}

static uint16_t tc_metadata_next_nonzero(uint16_t value)
{
    value++;
    return value == 0u ? 1u : value;
}

static int tc_metadata_candidate_equal(const tavrn_metadata_candidate_t *left,
                                       const tavrn_metadata_candidate_t *right)
{
    return left != NULL && right != NULL &&
        adva_equal(&left->subject, &right->subject) &&
        left->subject_sid8 == right->subject_sid8 &&
        left->ttl_bucket == right->ttl_bucket &&
        left->freshness_request == right->freshness_request &&
        left->departed == right->departed && left->kind == right->kind;
}

static int tc_metadata_candidate_valid(const tavrn_metadata_candidate_t *candidate)
{
    return candidate != NULL && tc_metadata_adva_valid(&candidate->subject) &&
        candidate->subject_sid8 != 0u && candidate->subject_sid8 != 0xffu &&
        candidate->subject_sid8 == candidate->subject.bytes[0] &&
        candidate->ttl_bucket <= 15u && candidate->freshness_request <= 1u &&
        candidate->departed == 0u &&
        candidate->kind <= TAVRN_METADATA_DELAYED_TARGETED_RESERVATION;
}

static void tc_metadata_purge_cooldown(
    tavrn_tc_metadata_cooldown_t cooldown[TAVRN_TC_METADATA_CANDIDATE_CAPACITY],
    uint32_t now_ms)
{
    uint8_t index;

    for (index = 0u; index < TAVRN_TC_METADATA_CANDIDATE_CAPACITY; index++) {
        if (cooldown[index].valid != 0u &&
            time_due(now_ms, cooldown[index].expires_at_ms)) {
            memset(&cooldown[index], 0, sizeof(cooldown[index]));
        }
    }
}

static void tc_metadata_purge(tavrn_tc_metadata_state_t *state, uint32_t now_ms)
{
    uint8_t index;

    for (index = 0u; index < TAVRN_TC_METADATA_UUID_CAPACITY; index++) {
        if (state->uuid[index].valid != 0u &&
            time_due(now_ms, state->uuid[index].expires_at_ms)) {
            memset(&state->uuid[index], 0, sizeof(state->uuid[index]));
        }
    }
    for (index = 0u; index < TAVRN_TC_METADATA_SUBJECT_CAPACITY; index++) {
        if (state->subject[index].valid != 0u &&
            time_due(now_ms, state->subject[index].expires_at_ms)) {
            memset(&state->subject[index], 0, sizeof(state->subject[index]));
        }
    }
    tc_metadata_purge_cooldown(state->cooldown, now_ms);
}

static int tc_metadata_origin_cause_event(tavrn_tc_origin_cause_t cause,
                                          tavrn_tc_event_t *event_out)
{
    if (event_out == NULL) {
        return 0;
    }
    if (cause == TAVRN_TC_ORIGIN_MENTORSHIP_SELF_JOIN) {
        *event_out = TAVRN_TC_EVENT_JOIN;
        return 1;
    }
    if (cause == TAVRN_TC_ORIGIN_VERIFICATION_TERMINAL_DEPARTURE ||
        cause == TAVRN_TC_ORIGIN_DIRECT_TIMEOUT_DEPARTURE ||
        cause == TAVRN_TC_ORIGIN_DATA_RETRY_EXHAUSTED) {
        *event_out = TAVRN_TC_EVENT_LEAVE;
        return 1;
    }
    return 0;
}

static void tc_metadata_build_action(tavrn_tc_metadata_state_t *state,
                                     const tavrn_adva_t *subject,
                                     tavrn_tc_event_t event, uint16_t sequence,
                                     tavrn_tc_metadata_action_t *action_out)
{
    memset(action_out, 0, sizeof(*action_out));
    action_out->origin = state->config.local_identity;
    action_out->subject = *subject;
    action_out->tc_sequence = sequence;
    action_out->ttl = 15u;
    action_out->hops = 0u;
    action_out->event = event;
    action_out->valid = 1u;
    action_out->pdu[0] = 0x54u;
    action_out->pdu[1] = 0x52u;
    action_out->pdu[2] = 0x02u;
    action_out->pdu[3] = state->config.network_id;
    action_out->pdu[4] = TAVRN_WIRE_TC_UPDATE;
    action_out->pdu[6] = 0xf0u;
    memcpy(&action_out->pdu[7], action_out->origin.bytes, TAVRN_ADVA_LEN);
    action_out->pdu[13] = (uint8_t)sequence;
    action_out->pdu[14] = (uint8_t)(sequence >> 8);
    memcpy(&action_out->pdu[15], action_out->subject.bytes, TAVRN_ADVA_LEN);
    action_out->pdu[21] = (uint8_t)event;
}

static void tc_metadata_preempt_rfi_for_local_fact(
    tavrn_maintenance_t *maintenance, uint32_t now_ms)
{
    if (maintenance != NULL && maintenance->local_tc_retained.callback != NULL) {
        maintenance->local_tc_retained.callback(
            maintenance->local_tc_retained.context, now_ms);
    }
}

static tavrn_tc_metadata_status_t tc_metadata_promote_origin(
    tavrn_tc_metadata_state_t *state)
{
    const tavrn_tc_metadata_origin_fact_t *fact;

    if (state->origin_pending.valid != 0u) {
        return TAVRN_TC_METADATA_RETAINED;
    }
    if (state->origin_fact_count == 0u) {
        return TAVRN_TC_METADATA_OK;
    }
    /* A mentorship JOIN owns its reserved ticket until its own enqueue has
     * committed.  A confirmed LEAVE remains retained as a fact, rather than
     * borrowing that sequence or being dropped. */
    if (state->sequence_ticket.valid != 0u) {
        return TAVRN_TC_METADATA_RETAINED;
    }
    fact = &state->origin_facts[0];
    if (fact->valid == 0u) {
        return TAVRN_TC_METADATA_INVALID;
    }
    tc_metadata_build_action(state, &fact->subject, fact->event,
                             state->next_tc_sequence, &state->origin_pending);
    return TAVRN_TC_METADATA_PREPARED;
}

static tavrn_tc_metadata_status_t tc_metadata_enqueue_origin_fact(
    tavrn_tc_metadata_state_t *state, const tavrn_adva_t *subject,
    tavrn_tc_event_t event)
{
    uint8_t index;
    tavrn_tc_metadata_status_t status;

    if (state->origin_pending.valid != 0u &&
        state->origin_pending.event == event &&
        adva_equal(&state->origin_pending.subject, subject)) {
        return TAVRN_TC_METADATA_RETAINED;
    }
    for (index = 0u; index < state->origin_fact_count; index++) {
        if (state->origin_facts[index].valid != 0u &&
            state->origin_facts[index].event == event &&
            adva_equal(&state->origin_facts[index].subject, subject)) {
            return TAVRN_TC_METADATA_RETAINED;
        }
    }
    if (state->origin_fact_count >= TAVRN_TC_METADATA_ORIGIN_CAPACITY) {
        state->origin_busy_count++;
        return TAVRN_TC_METADATA_BUSY;
    }
    state->origin_facts[state->origin_fact_count].subject = *subject;
    state->origin_facts[state->origin_fact_count].event = event;
    state->origin_facts[state->origin_fact_count].valid = 1u;
    state->origin_fact_count++;
    status = tc_metadata_promote_origin(state);
    return status == TAVRN_TC_METADATA_OK ? TAVRN_TC_METADATA_RETAINED : status;
}

static int tc_metadata_origin_fact_is_retained(
    const tavrn_tc_metadata_state_t *state, const tavrn_adva_t *subject,
    tavrn_tc_event_t event)
{
    uint8_t index;

    if (state->origin_pending.valid != 0u &&
        state->origin_pending.event == event &&
        adva_equal(&state->origin_pending.subject, subject)) {
        return 1;
    }
    for (index = 0u; index < state->origin_fact_count; index++) {
        if (state->origin_facts[index].valid != 0u &&
            state->origin_facts[index].event == event &&
            adva_equal(&state->origin_facts[index].subject, subject)) {
            return 1;
        }
    }
    return 0;
}

static tavrn_tc_metadata_status_t tc_metadata_enqueue_retry_exhausted_leave(
    tavrn_tc_metadata_state_t *state, const tavrn_adva_t *failed_next_hop)
{
    tavrn_tc_metadata_retry_exhausted_leave_overflow_t *overflow;

    if (tc_metadata_origin_fact_is_retained(state, failed_next_hop,
                                            TAVRN_TC_EVENT_LEAVE)) {
        return TAVRN_TC_METADATA_RETAINED;
    }
    if (state->origin_fact_count < TAVRN_TC_METADATA_ORIGIN_CAPACITY) {
        return tc_metadata_enqueue_origin_fact(state, failed_next_hop,
                                               TAVRN_TC_EVENT_LEAVE);
    }
    overflow = &state->retry_exhausted_leave_overflow;
    if (overflow->valid != 0u) {
        return adva_equal(&overflow->failed_next_hop, failed_next_hop) ?
            TAVRN_TC_METADATA_RETAINED : TAVRN_TC_METADATA_INVALID;
    }
    overflow->failed_next_hop = *failed_next_hop;
    overflow->valid = 1u;
    state->origin_busy_count++;
    return TAVRN_TC_METADATA_RETAINED;
}

static void tc_metadata_promote_retry_exhausted_leave_overflow(
    tavrn_tc_metadata_state_t *state)
{
    tavrn_tc_metadata_retry_exhausted_leave_overflow_t *overflow =
        &state->retry_exhausted_leave_overflow;

    if (overflow->valid == 0u ||
        state->origin_fact_count >= TAVRN_TC_METADATA_ORIGIN_CAPACITY) {
        return;
    }
    state->origin_facts[state->origin_fact_count].subject =
        overflow->failed_next_hop;
    state->origin_facts[state->origin_fact_count].event = TAVRN_TC_EVENT_LEAVE;
    state->origin_facts[state->origin_fact_count].valid = 1u;
    state->origin_fact_count++;
    memset(overflow, 0, sizeof(*overflow));
}

static void tc_metadata_commit_origin_fact(tavrn_tc_metadata_state_t *state,
                                            const tavrn_tc_metadata_action_t *action)
{
    uint8_t index;

    if (state->origin_fact_count != 0u &&
        state->origin_facts[0].valid != 0u &&
        state->origin_facts[0].event == action->event &&
        adva_equal(&state->origin_facts[0].subject, &action->subject)) {
        for (index = 1u; index < state->origin_fact_count; index++) {
            state->origin_facts[(uint8_t)(index - 1u)] = state->origin_facts[index];
        }
        memset(&state->origin_facts[(uint8_t)(state->origin_fact_count - 1u)], 0,
               sizeof(state->origin_facts[0]));
        state->origin_fact_count--;
    }
}

static int tc_metadata_relay_has_capacity(const tavrn_tc_metadata_state_t *state)
{
    return state->relay_queue_count < TAVRN_TC_METADATA_RELAY_CAPACITY;
}

static void tc_metadata_enqueue_relay(tavrn_tc_metadata_state_t *state,
                                      const tavrn_tc_metadata_action_t *action)
{
    if (state->relay_queue_count == 0u) {
        state->relay_pending = *action;
    } else {
        state->relay_backlog[(uint8_t)(state->relay_queue_count - 1u)] = *action;
    }
    state->relay_queue_count++;
}

static void tc_metadata_commit_relay_action(tavrn_tc_metadata_state_t *state)
{
    uint8_t index;

    if (state->relay_queue_count <= 1u) {
        state->relay_queue_count = 0u;
        memset(&state->relay_pending, 0, sizeof(state->relay_pending));
        return;
    }
    state->relay_pending = state->relay_backlog[0];
    for (index = 1u; index < state->relay_queue_count - 1u; index++) {
        state->relay_backlog[(uint8_t)(index - 1u)] = state->relay_backlog[index];
    }
    memset(&state->relay_backlog[(uint8_t)(state->relay_queue_count - 2u)], 0,
           sizeof(state->relay_backlog[0]));
    state->relay_queue_count--;
}

tavrn_tc_metadata_status_t tavrn_maintenance_tc_metadata_init(
    tavrn_tc_metadata_state_t *state, const tavrn_tc_metadata_config_t *config)
{
    if (state == NULL || config == NULL || !tc_metadata_adva_valid(&config->local_identity) ||
        config->tc_uuid_ms == 0u || config->tc_uuid_ms >= TAVRN_MAINTENANCE_HALF_RANGE ||
        config->tc_subject_ms == 0u || config->tc_subject_ms >= TAVRN_MAINTENANCE_HALF_RANGE ||
        config->metadata_cooldown_ms == 0u ||
        config->metadata_cooldown_ms >= TAVRN_MAINTENANCE_HALF_RANGE) {
        return TAVRN_TC_METADATA_INVALID;
    }
    memset(state, 0, sizeof(*state));
    state->config = *config;
    state->next_tc_sequence = config->initial_tc_sequence == 0u ? 1u :
        config->initial_tc_sequence;
    state->initialized = 1u;
    return TAVRN_TC_METADATA_OK;
}

tavrn_tc_metadata_status_t tavrn_maintenance_tc_metadata_bind_gtt(
    tavrn_tc_metadata_state_t *state, tavrn_gtt_t *gtt)
{
    if (!tc_metadata_state_valid(state) || gtt == NULL || gtt->storage == NULL ||
        !adva_equal(&state->config.local_identity, &gtt->config.local_identity)) {
        return TAVRN_TC_METADATA_INVALID;
    }
    state->gtt = gtt;
    return TAVRN_TC_METADATA_OK;
}

tavrn_tc_metadata_status_t tavrn_maintenance_tc_sequence_prepare(
    tavrn_tc_metadata_state_t *state, tavrn_tc_event_t event,
    tavrn_tc_sequence_ticket_t *ticket_out)
{
    if (ticket_out != NULL) {
        memset(ticket_out, 0, sizeof(*ticket_out));
    }
    if (!tc_metadata_state_valid(state) || ticket_out == NULL ||
        (event != TAVRN_TC_EVENT_JOIN && event != TAVRN_TC_EVENT_LEAVE)) {
        return TAVRN_TC_METADATA_INVALID;
    }
    if (state->sequence_ticket.valid != 0u) {
        *ticket_out = state->sequence_ticket;
        return state->sequence_ticket.event == event ? TAVRN_TC_METADATA_RETAINED :
                                                       TAVRN_TC_METADATA_BUSY;
    }
    state->sequence_ticket.sequence = state->next_tc_sequence;
    state->sequence_ticket.event = event;
    state->sequence_ticket.valid = 1u;
    *ticket_out = state->sequence_ticket;
    return TAVRN_TC_METADATA_PREPARED;
}

tavrn_tc_metadata_status_t tavrn_maintenance_tc_sequence_commit(
    tavrn_tc_metadata_state_t *state, const tavrn_tc_sequence_ticket_t *ticket,
    tavrn_tc_admission_t admission, uint32_t now_ms)
{
    (void)now_ms;
    if (!tc_metadata_state_valid(state) || ticket == NULL || ticket->valid == 0u ||
        state->sequence_ticket.valid == 0u ||
        ticket->sequence != state->sequence_ticket.sequence ||
        ticket->event != state->sequence_ticket.event) {
        return TAVRN_TC_METADATA_INVALID;
    }
    if (admission != TAVRN_TC_ADMISSION_ADMITTED) {
        return TAVRN_TC_METADATA_RETAINED;
    }
    state->next_tc_sequence = tc_metadata_next_nonzero(ticket->sequence);
    memset(&state->sequence_ticket, 0, sizeof(state->sequence_ticket));
    return TAVRN_TC_METADATA_OK;
}

tavrn_tc_metadata_status_t tavrn_maintenance_tc_prepare_origin(
    tavrn_tc_metadata_state_t *state, tavrn_tc_origin_cause_t cause,
    const tavrn_adva_t *subject, const tavrn_adva_t *failed_next_hop_or_null,
    const tavrn_adva_t *final_destination_or_null, uint32_t now_ms,
    tavrn_tc_metadata_action_t *action_out)
{
    tavrn_tc_event_t event;
    const tavrn_adva_t *effective_subject = subject;

    (void)final_destination_or_null;
    if (action_out != NULL) {
        memset(action_out, 0, sizeof(*action_out));
    }
    if (!tc_metadata_state_valid(state) || action_out == NULL ||
        !tc_metadata_origin_cause_event(cause, &event)) {
        return tc_metadata_state_valid(state) ? TAVRN_TC_METADATA_IGNORED :
                                                TAVRN_TC_METADATA_INVALID;
    }
    if (cause == TAVRN_TC_ORIGIN_DATA_RETRY_EXHAUSTED) {
        effective_subject = failed_next_hop_or_null;
    }
    if (!tc_metadata_adva_valid(effective_subject)) {
        return TAVRN_TC_METADATA_INVALID;
    }
    if (state->origin_pending.valid != 0u) {
        if (state->origin_pending.event == event &&
            adva_equal(&state->origin_pending.subject, effective_subject)) {
            *action_out = state->origin_pending;
            return TAVRN_TC_METADATA_RETAINED;
        }
        return TAVRN_TC_METADATA_BUSY;
    }
    if (state->sequence_ticket.valid != 0u) {
        if (state->sequence_ticket.event != event) {
            return TAVRN_TC_METADATA_BUSY;
        }
        tc_metadata_build_action(state, effective_subject, event,
                                 state->sequence_ticket.sequence, action_out);
        return TAVRN_TC_METADATA_RETAINED;
    }
    /* Preparing a source fact is side-effect free.  The generic TC sequence is
     * reserved only when the owner actually attempts its containing control, so
     * independent confirmed facts cannot consume or block the serial stream. */
    tc_metadata_build_action(state, effective_subject, event, state->next_tc_sequence,
                             action_out);
    (void)now_ms;
    return TAVRN_TC_METADATA_PREPARED;
}

tavrn_tc_metadata_status_t tavrn_maintenance_tc_commit_origin(
    tavrn_tc_metadata_state_t *state, const tavrn_tc_metadata_action_t *action,
    tavrn_tc_admission_t admission, uint32_t now_ms)
{
    tavrn_tc_sequence_ticket_t ticket;
    tavrn_tc_metadata_status_t status;

    if (!tc_metadata_state_valid(state) || action == NULL || action->valid == 0u ||
        !adva_equal(&action->origin, &state->config.local_identity) ||
        !tc_metadata_adva_valid(&action->subject) ||
        (action->event != TAVRN_TC_EVENT_JOIN && action->event != TAVRN_TC_EVENT_LEAVE)) {
        return TAVRN_TC_METADATA_INVALID;
    }
    if (state->origin_pending.valid != 0u &&
        memcmp(action, &state->origin_pending, sizeof(*action)) != 0) {
        return TAVRN_TC_METADATA_INVALID;
    }
    if (state->sequence_ticket.valid == 0u) {
        status = tavrn_maintenance_tc_sequence_prepare(state, action->event, &ticket);
        if (status != TAVRN_TC_METADATA_PREPARED || ticket.sequence != action->tc_sequence) {
            return status == TAVRN_TC_METADATA_BUSY ? status : TAVRN_TC_METADATA_INVALID;
        }
    }
    ticket.sequence = action->tc_sequence;
    ticket.event = action->event;
    ticket.valid = 1u;
    status = tavrn_maintenance_tc_sequence_commit(state, &ticket, admission, now_ms);
    if (status == TAVRN_TC_METADATA_OK) {
        state->admission_count++;
        state->last_tc_sequence = action->tc_sequence;
        state->last_tc_event = action->event;
        state->last_subject_sid8 = action->subject.bytes[0];
        memset(&state->origin_pending, 0, sizeof(state->origin_pending));
    } else if (status == TAVRN_TC_METADATA_RETAINED && state->origin_pending.valid == 0u) {
        state->origin_busy_count++;
        state->origin_pending = *action;
    }
    return status;
}

static int tc_metadata_tc_pdu_valid(const tavrn_tc_metadata_state_t *state,
                                    const uint8_t pdu[TAVRN_LINK_CONTROL_PDU_MAX],
                                    uint8_t pdu_len, tavrn_adva_t *origin_out,
                                    tavrn_adva_t *subject_out, uint16_t *sequence_out,
                                    tavrn_tc_event_t *event_out)
{
    uint8_t ttl;
    uint8_t hops;

    if (state == NULL || pdu == NULL || pdu_len != TAVRN_LINK_CONTROL_PDU_MAX ||
        pdu[0] != 0x54u || pdu[1] != 0x52u || pdu[2] != 0x02u ||
        (state->config.network_id != 0u && pdu[3] != state->config.network_id) ||
        pdu[4] != TAVRN_WIRE_TC_UPDATE || pdu[5] != 0u || pdu[21] > 1u) {
        return 0;
    }
    ttl = (uint8_t)(pdu[6] >> 4);
    hops = (uint8_t)(pdu[6] & 0x0fu);
    if (ttl > 15u || hops > 15u) {
        return 0;
    }
    memcpy(origin_out->bytes, &pdu[7], TAVRN_ADVA_LEN);
    memcpy(subject_out->bytes, &pdu[15], TAVRN_ADVA_LEN);
    if (!tc_metadata_adva_valid(origin_out) || !tc_metadata_adva_valid(subject_out)) {
        return 0;
    }
    *sequence_out = (uint16_t)pdu[13] | ((uint16_t)pdu[14] << 8);
    *event_out = (tavrn_tc_event_t)pdu[21];
    return 1;
}

static int tc_metadata_uuid_find_or_free(tavrn_tc_metadata_state_t *state,
                                         const tavrn_adva_t *origin,
                                         uint16_t sequence, uint32_t now_ms,
                                         uint8_t *duplicate_out)
{
    uint8_t index;
    int free_slot = -1;

    *duplicate_out = 0u;
    tc_metadata_purge(state, now_ms);
    for (index = 0u; index < TAVRN_TC_METADATA_UUID_CAPACITY; index++) {
        if (state->uuid[index].valid != 0u &&
            state->uuid[index].sequence == sequence &&
            adva_equal(&state->uuid[index].origin, origin)) {
            *duplicate_out = 1u;
            return (int)index;
        }
        if (state->uuid[index].valid == 0u && free_slot < 0) {
            free_slot = (int)index;
        }
    }
    return free_slot;
}

static int tc_metadata_subject_find_or_free(tavrn_tc_metadata_state_t *state,
                                            const tavrn_adva_t *subject,
                                            tavrn_tc_event_t event,
                                            uint32_t now_ms, uint8_t *duplicate_out)
{
    uint8_t index;
    int free_slot = -1;

    *duplicate_out = 0u;
    tc_metadata_purge(state, now_ms);
    for (index = 0u; index < TAVRN_TC_METADATA_SUBJECT_CAPACITY; index++) {
        if (state->subject[index].valid != 0u && state->subject[index].event == event &&
            adva_equal(&state->subject[index].subject, subject)) {
            *duplicate_out = 1u;
            return (int)index;
        }
        if (state->subject[index].valid == 0u && free_slot < 0) {
            free_slot = (int)index;
        }
    }
    return free_slot;
}

static tavrn_tc_metadata_status_t tc_metadata_apply_imported(
    tavrn_tc_metadata_state_t *state, const tavrn_adva_t *subject,
    tavrn_tc_event_t event, uint8_t hop_count, uint32_t now_ms)
{
    tavrn_gtt_evidence_t evidence;
    tavrn_gtt_expiry_observe_status_t observe;

    if (state->gtt == NULL) {
        state->imported_non_direct_count++;
        state->local_apply_count++;
        return TAVRN_TC_METADATA_APPLIED;
    }
    memset(&evidence, 0, sizeof(evidence));
    evidence.identity = *subject;
    /* TC sequence identifies the flood UUID {origin, sequence}; it is not a
     * subject-owned GTT freshness serial. */
    evidence.serial_present = 0u;
    evidence.hop_count = hop_count;
    evidence.kind = event == TAVRN_TC_EVENT_LEAVE ? TAVRN_GTT_EVIDENCE_DEPARTED :
                                                   TAVRN_GTT_EVIDENCE_LIVENESS;
    observe = tavrn_gtt_observe_with_provenance(
        state->gtt, &evidence,
        event == TAVRN_TC_EVENT_LEAVE ? TAVRN_GTT_PROVENANCE_CONFIRMED_DEPARTURE :
                                       TAVRN_GTT_PROVENANCE_IMPORTED_TC_SUBJECT,
        now_ms, NULL);
    if (observe != TAVRN_GTT_EXPIRY_OBSERVE_ADDED &&
        observe != TAVRN_GTT_EXPIRY_OBSERVE_REFRESHED &&
        observe != TAVRN_GTT_EXPIRY_OBSERVE_REPLACED_DEPARTED &&
        observe != TAVRN_GTT_EXPIRY_OBSERVE_REPLACED_OLDEST_STALE &&
        observe != TAVRN_GTT_EXPIRY_OBSERVE_DUPLICATE &&
        observe != TAVRN_GTT_EXPIRY_OBSERVE_SELF_IGNORED) {
        return TAVRN_TC_METADATA_BUSY;
    }
    state->imported_non_direct_count++;
    state->local_apply_count++;
    return TAVRN_TC_METADATA_APPLIED;
}

static void tc_metadata_observe_outer_transmitter(
    tavrn_tc_metadata_state_t *state, const tavrn_adva_t *outer_transmitter,
    uint32_t now_ms)
{
    tavrn_gtt_evidence_t evidence;

    if (state->gtt == NULL) {
        return;
    }
    memset(&evidence, 0, sizeof(evidence));
    evidence.identity = *outer_transmitter;
    evidence.hop_count = 1u;
    evidence.kind = TAVRN_GTT_EVIDENCE_LIVENESS;
    (void)tavrn_gtt_observe_with_provenance(
        state->gtt, &evidence, TAVRN_GTT_PROVENANCE_DIRECT_OUTER_TRANSMITTER,
        now_ms, NULL);
}

tavrn_tc_metadata_status_t tavrn_maintenance_tc_receive(
    tavrn_tc_metadata_state_t *state, const uint8_t pdu[TAVRN_LINK_CONTROL_PDU_MAX],
    uint8_t pdu_len, const tavrn_adva_t *outer_transmitter, uint32_t now_ms,
    tavrn_tc_metadata_action_t *relay_out)
{
    tavrn_adva_t origin;
    tavrn_adva_t subject;
    tavrn_tc_event_t event;
    uint16_t sequence;
    uint8_t duplicate_uuid;
    uint8_t duplicate_subject;
    uint8_t ttl;
    uint8_t hops;
    int uuid_slot;
    int subject_slot;
    tavrn_tc_metadata_status_t apply_status;

    if (relay_out != NULL) {
        memset(relay_out, 0, sizeof(*relay_out));
    }
    if (!tc_metadata_state_valid(state) || outer_transmitter == NULL || relay_out == NULL ||
        !tc_metadata_adva_valid(outer_transmitter) ||
        !tc_metadata_tc_pdu_valid(state, pdu, pdu_len, &origin, &subject, &sequence,
                                  &event)) {
        return TAVRN_TC_METADATA_INVALID;
    }
    /* TC is intercepted before generic router observation.  Commit only the
     * validated immediate transmitter as direct; origin/subject stay TC-owned. */
    tc_metadata_observe_outer_transmitter(state, outer_transmitter, now_ms);
    ttl = (uint8_t)(pdu[6] >> 4);
    hops = (uint8_t)(pdu[6] & 0x0fu);
    uuid_slot = tc_metadata_uuid_find_or_free(state, &origin, sequence, now_ms,
                                               &duplicate_uuid);
    if (duplicate_uuid != 0u) {
        state->duplicate_count++;
        return TAVRN_TC_METADATA_IGNORED;
    }
    if (uuid_slot < 0) {
        state->relay_busy_count++;
        return TAVRN_TC_METADATA_BUSY;
    }
    subject_slot = tc_metadata_subject_find_or_free(state, &subject, event, now_ms,
                                                     &duplicate_subject);
    if (subject_slot < 0) {
        state->relay_busy_count++;
        return TAVRN_TC_METADATA_BUSY;
    }
    /* A duplicate UUID or a subject-suppressed event has no relay obligation,
     * so a full relay FIFO must not prevent its cache semantics.  A fresh,
     * relayable fact preflights capacity before any local apply/cache mutation. */
    if (duplicate_subject == 0u && ttl != 0u && hops < 15u &&
        !tc_metadata_relay_has_capacity(state)) {
        state->relay_busy_count++;
        return TAVRN_TC_METADATA_BUSY;
    }
    /* Complete validation and capacity reservation happened above.  UUID is
     * intentionally retained before subject suppression. */
    state->uuid[(uint8_t)uuid_slot].origin = origin;
    state->uuid[(uint8_t)uuid_slot].sequence = sequence;
    state->uuid[(uint8_t)uuid_slot].expires_at_ms = now_ms + state->config.tc_uuid_ms;
    state->uuid[(uint8_t)uuid_slot].valid = 1u;
    if (duplicate_subject != 0u) {
        state->duplicate_count++;
        return TAVRN_TC_METADATA_IGNORED;
    }
    state->subject[(uint8_t)subject_slot].subject = subject;
    state->subject[(uint8_t)subject_slot].event = event;
    state->subject[(uint8_t)subject_slot].expires_at_ms = now_ms +
        state->config.tc_subject_ms;
    state->subject[(uint8_t)subject_slot].valid = 1u;
    apply_status = tc_metadata_apply_imported(state, &subject, event,
                                              hops == 15u ? 15u : (uint8_t)(hops + 1u),
                                              now_ms);
    if (apply_status != TAVRN_TC_METADATA_APPLIED) {
        memset(&state->subject[(uint8_t)subject_slot], 0,
               sizeof(state->subject[(uint8_t)subject_slot]));
        memset(&state->uuid[(uint8_t)uuid_slot], 0, sizeof(state->uuid[(uint8_t)uuid_slot]));
        return apply_status;
    }
    /* The four-bit hop field cannot represent 16.  A fully valid terminal-hop
     * TC still applies once, but relay suppression prevents a nibble carry
     * from corrupting TTL or restarting gossip. */
    if (ttl == 0u || hops == 15u) {
        return TAVRN_TC_METADATA_APPLIED;
    }
    memset(relay_out, 0, sizeof(*relay_out));
    memcpy(relay_out->pdu, pdu, TAVRN_LINK_CONTROL_PDU_MAX);
    relay_out->pdu[6] = (uint8_t)(((ttl - 1u) << 4) | (hops + 1u));
    relay_out->origin = origin;
    relay_out->subject = subject;
    relay_out->tc_sequence = sequence;
    relay_out->ttl = (uint8_t)(ttl - 1u);
    relay_out->hops = (uint8_t)(hops + 1u);
    relay_out->event = event;
    relay_out->valid = 1u;
    tc_metadata_enqueue_relay(state, relay_out);
    return TAVRN_TC_METADATA_APPLIED;
}

tavrn_tc_metadata_status_t tavrn_maintenance_tc_commit_relay(
    tavrn_tc_metadata_state_t *state, const tavrn_tc_metadata_action_t *action,
    tavrn_tc_admission_t admission, uint32_t now_ms)
{
    (void)now_ms;
    if (!tc_metadata_state_valid(state) || action == NULL || action->valid == 0u ||
        state->relay_pending.valid == 0u ||
        memcmp(action, &state->relay_pending, sizeof(*action)) != 0) {
        return TAVRN_TC_METADATA_INVALID;
    }
    if (admission != TAVRN_TC_ADMISSION_ADMITTED) {
        state->relay_busy_count++;
        return TAVRN_TC_METADATA_RETAINED;
    }
    state->relay_count++;
    state->admission_count++;
    state->last_tc_sequence = action->tc_sequence;
    state->last_tc_event = action->event;
    state->last_subject_sid8 = action->subject.bytes[0];
    tc_metadata_commit_relay_action(state);
    return TAVRN_TC_METADATA_OK;
}

static uint8_t tc_metadata_frame_capacity(tavrn_metadata_frame_kind_t kind,
                                          uint8_t unreachable_count)
{
    if (kind == TAVRN_METADATA_FRAME_RREQ8) return 4u;
    if (kind == TAVRN_METADATA_FRAME_RREP8) return 3u;
    if (kind != TAVRN_METADATA_FRAME_RERR8) return 0xffu;
    if (unreachable_count == 1u) return 4u;
    if (unreachable_count == 2u) return 3u;
    if (unreachable_count == 3u) return 1u;
    if (unreachable_count == 4u) return 0u;
    return 0xffu;
}

static uint8_t tc_metadata_candidate_priority(const tavrn_metadata_candidate_t *candidate)
{
    if (candidate->kind == TAVRN_METADATA_SOFT_REQUEST) return 0u;
    if (candidate->kind == TAVRN_METADATA_SUBJECT_SELF_ANSWER) return 1u;
    if (candidate->kind == TAVRN_METADATA_INTERMEDIARY_ANSWER) return 2u;
    return 0xffu;
}

static tavrn_tc_metadata_status_t tc_metadata_create_in(
    const tavrn_tc_metadata_state_t *state,
    tavrn_tc_metadata_slot_t slots[TAVRN_TC_METADATA_CANDIDATE_CAPACITY],
    tavrn_tc_metadata_cooldown_t cooldown[TAVRN_TC_METADATA_CANDIDATE_CAPACITY],
    const tavrn_metadata_candidate_t *candidate, uint32_t now_ms)
{
    uint8_t index;
    int free_slot = -1;

    if (!tc_metadata_state_valid(state) || !tc_metadata_candidate_valid(candidate)) {
        return TAVRN_TC_METADATA_INVALID;
    }
    tc_metadata_purge_cooldown(cooldown, now_ms);
    for (index = 0u; index < TAVRN_TC_METADATA_CANDIDATE_CAPACITY; index++) {
        tavrn_tc_metadata_slot_t *slot = &slots[index];

        if (slot->valid != 0u && tc_metadata_candidate_equal(&slot->candidate, candidate)) {
            return TAVRN_TC_METADATA_OK;
        }
        if (slot->valid == 0u && free_slot < 0) {
            free_slot = (int)index;
        }
        if (cooldown[index].valid != 0u &&
            tc_metadata_candidate_equal(&cooldown[index].candidate, candidate)) {
            /* Identical semantic work stays suppressed until due-at-equality
             * expiry.  Returning OK preserves callers' idempotent semantics. */
            return TAVRN_TC_METADATA_OK;
        }
        if (cooldown[index].valid != 0u &&
            cooldown[index].candidate.kind == candidate->kind &&
            adva_equal(&cooldown[index].candidate.subject, &candidate->subject)) {
            /* A changed bucket/request state is newer semantic evidence and
             * invalidates only its obsolete cooldown record. */
            memset(&cooldown[index], 0, sizeof(cooldown[index]));
        }
    }
    if (free_slot < 0) {
        return TAVRN_TC_METADATA_BUSY;
    }
    memset(&slots[(uint8_t)free_slot], 0, sizeof(slots[(uint8_t)free_slot]));
    slots[(uint8_t)free_slot].candidate = *candidate;
    slots[(uint8_t)free_slot].valid = 1u;
    return TAVRN_TC_METADATA_OK;
}

tavrn_tc_metadata_status_t tavrn_maintenance_metadata_create(
    tavrn_tc_metadata_state_t *state, const tavrn_metadata_candidate_t *candidate,
    uint32_t now_ms)
{
    if (!tc_metadata_state_valid(state)) {
        return TAVRN_TC_METADATA_INVALID;
    }
    tc_metadata_purge(state, now_ms);
    return tc_metadata_create_in(state, state->slots, state->cooldown, candidate, now_ms);
}

tavrn_tc_metadata_status_t tavrn_maintenance_metadata_select(
    const tavrn_tc_metadata_state_t *state, tavrn_metadata_frame_kind_t frame_kind,
    uint8_t unreachable_count, uint32_t now_ms,
    tavrn_metadata_selection_t *selection_out)
{
    uint8_t capacity;
    uint8_t cooldown_capacity = 0u;
    uint8_t priority;

    if (selection_out != NULL) {
        memset(selection_out, 0, sizeof(*selection_out));
    }
    if (!tc_metadata_state_valid(state) || selection_out == NULL) {
        return TAVRN_TC_METADATA_INVALID;
    }
    capacity = tc_metadata_frame_capacity(frame_kind, unreachable_count);
    if (capacity == 0xffu) {
        return TAVRN_TC_METADATA_INVALID;
    }
    for (priority = 0u; priority < TAVRN_TC_METADATA_CANDIDATE_CAPACITY;
         priority++) {
        if (state->cooldown[priority].valid == 0u ||
            time_due(now_ms, state->cooldown[priority].expires_at_ms)) {
            cooldown_capacity++;
        }
    }
    if (capacity > cooldown_capacity) {
        capacity = cooldown_capacity;
    }
    selection_out->capacity = capacity;
    selection_out->valid = 1u;
    for (priority = 0u; priority < 3u && selection_out->count < capacity; priority++) {
        uint8_t offset;

        for (offset = 0u; offset < TAVRN_TC_METADATA_CANDIDATE_CAPACITY &&
             selection_out->count < capacity; offset++) {
            uint8_t index = (uint8_t)((state->round_robin_cursor + offset) %
                                      TAVRN_TC_METADATA_CANDIDATE_CAPACITY);
            const tavrn_tc_metadata_slot_t *slot = &state->slots[index];

            if (slot->valid != 0u &&
                tc_metadata_candidate_priority(&slot->candidate) == priority) {
                selection_out->entries[selection_out->count++] = slot->candidate;
            }
        }
    }
    (void)now_ms;
    return TAVRN_TC_METADATA_OK;
}

tavrn_tc_metadata_status_t tavrn_maintenance_metadata_commit(
    tavrn_tc_metadata_state_t *state, const tavrn_metadata_selection_t *selection,
    tavrn_tc_admission_t admission, uint32_t now_ms)
{
    uint8_t index;
    uint8_t matched_slots[TAVRN_TC_METADATA_CANDIDATE_CAPACITY];
    uint8_t cooldown_slots[TAVRN_TC_METADATA_CANDIDATE_CAPACITY];
    uint8_t released = 0u;

    if (!tc_metadata_state_valid(state) || selection == NULL || selection->valid == 0u ||
        selection->count > TAVRN_TC_METADATA_CANDIDATE_CAPACITY) {
        return TAVRN_TC_METADATA_INVALID;
    }
    if (admission != TAVRN_TC_ADMISSION_ADMITTED) {
        return TAVRN_TC_METADATA_RETAINED;
    }
    tc_metadata_purge(state, now_ms);
    for (index = 0u; index < selection->count; index++) {
        uint8_t slot_index;
        int matched = -1;
        int cooldown_slot = -1;

        for (slot_index = 0u; slot_index < TAVRN_TC_METADATA_CANDIDATE_CAPACITY;
             slot_index++) {
            if (state->slots[slot_index].valid != 0u &&
                state->slots[slot_index].candidate.kind !=
                    TAVRN_METADATA_DELAYED_TARGETED_RESERVATION &&
                tc_metadata_candidate_equal(&state->slots[slot_index].candidate,
                                            &selection->entries[index])) {
                matched = (int)slot_index;
                break;
            }
        }
        if (matched < 0) {
            return TAVRN_TC_METADATA_INVALID;
        }
        for (slot_index = 0u; slot_index < index; slot_index++) {
            if (matched_slots[slot_index] == (uint8_t)matched) {
                return TAVRN_TC_METADATA_INVALID;
            }
        }
        for (slot_index = 0u; slot_index < TAVRN_TC_METADATA_CANDIDATE_CAPACITY;
             slot_index++) {
            uint8_t prior;
            uint8_t already_selected = 0u;

            for (prior = 0u; prior < index; prior++) {
                if (cooldown_slots[prior] == slot_index) {
                    already_selected = 1u;
                    break;
                }
            }
            if (already_selected == 0u && state->cooldown[slot_index].valid == 0u) {
                cooldown_slot = (int)slot_index;
                break;
            }
        }
        if (cooldown_slot < 0) {
            return TAVRN_TC_METADATA_BUSY;
        }
        matched_slots[index] = (uint8_t)matched;
        cooldown_slots[index] = (uint8_t)cooldown_slot;
    }
    for (index = 0u; index < selection->count; index++) {
        state->cooldown[cooldown_slots[index]].candidate =
            state->slots[matched_slots[index]].candidate;
        state->cooldown[cooldown_slots[index]].expires_at_ms =
            now_ms + state->config.metadata_cooldown_ms;
        state->cooldown[cooldown_slots[index]].valid = 1u;
        memset(&state->slots[matched_slots[index]], 0,
               sizeof(state->slots[matched_slots[index]]));
        released++;
    }
    if (released != 0u) {
        state->round_robin_cursor = (uint8_t)((state->round_robin_cursor + released) %
                                              TAVRN_TC_METADATA_CANDIDATE_CAPACITY);
    }
    return TAVRN_TC_METADATA_OK;
}

static int tc_metadata_control_base(const tavrn_validated_control_t *base,
                                    tavrn_metadata_frame_kind_t *kind_out,
                                    uint8_t *capacity_out, uint8_t *offset_out)
{
    uint8_t count;
    uint8_t base_len;
    uint8_t metadata_mask;

    if (base == NULL || kind_out == NULL || capacity_out == NULL || offset_out == NULL ||
        (base->pdu[5] & 0x80u) == 0u) {
        return 0;
    }
    if (base->type == TAVRN_WIRE_E_RREQ) {
        *kind_out = TAVRN_METADATA_FRAME_RREQ8;
        *capacity_out = 4u;
        base_len = 15u;
        metadata_mask = 0x08u;
    } else if (base->type == TAVRN_WIRE_E_RREP) {
        *kind_out = TAVRN_METADATA_FRAME_RREP8;
        *capacity_out = 3u;
        base_len = 16u;
        metadata_mask = 0x08u;
    } else if (base->type == TAVRN_WIRE_E_RERR) {
        count = base->pdu[10];
        *kind_out = TAVRN_METADATA_FRAME_RERR8;
        *capacity_out = tc_metadata_frame_capacity(*kind_out, count);
        if (*capacity_out == 0xffu) return 0;
        base_len = (uint8_t)(11u + 3u * count);
        metadata_mask = 0x10u;
    } else {
        return 0;
    }
    if (base->pdu_len != base_len || (base->pdu[5] & metadata_mask) != 0u) {
        return 0;
    }
    *offset_out = base_len;
    return 1;
}

tavrn_tc_metadata_status_t tavrn_maintenance_metadata_attach_control(
    const tavrn_validated_control_t *base, const tavrn_metadata_selection_t *selection,
    tavrn_metadata_attached_control_t *attached_out)
{
    tavrn_metadata_frame_kind_t frame_kind;
    uint8_t capacity;
    uint8_t offset;
    uint8_t emit;
    uint8_t index;

    if (attached_out != NULL) {
        memset(attached_out, 0, sizeof(*attached_out));
    }
    if (base == NULL || selection == NULL || attached_out == NULL ||
        selection->valid == 0u || selection->count > TAVRN_TC_METADATA_CANDIDATE_CAPACITY ||
        !tc_metadata_control_base(base, &frame_kind, &capacity, &offset)) {
        return TAVRN_TC_METADATA_INVALID;
    }
    attached_out->control = *base;
    attached_out->emitted.valid = 1u;
    attached_out->emitted.capacity = capacity;
    emit = selection->count < capacity ? selection->count : capacity;
    for (index = 0u; index < emit; index++) {
        const tavrn_metadata_candidate_t *entry = &selection->entries[index];

        if (!tc_metadata_candidate_valid(entry) ||
            entry->kind == TAVRN_METADATA_DELAYED_TARGETED_RESERVATION) {
            return TAVRN_TC_METADATA_INVALID;
        }
        attached_out->emitted.entries[index] = *entry;
    }
    attached_out->emitted.count = emit;
    if (emit == 0u) {
        return TAVRN_TC_METADATA_OK;
    }
    attached_out->control.pdu[5] |= frame_kind == TAVRN_METADATA_FRAME_RERR8 ?
        0x10u : 0x08u;
    attached_out->control.pdu[offset] = emit;
    for (index = 0u; index < emit; index++) {
        const tavrn_metadata_candidate_t *entry = &attached_out->emitted.entries[index];
        uint8_t entry_offset = (uint8_t)(offset + 1u + 2u * index);

        attached_out->control.pdu[entry_offset] = entry->subject_sid8;
        attached_out->control.pdu[(uint8_t)(entry_offset + 1u)] =
            (uint8_t)((entry->ttl_bucket << 4) | entry->freshness_request);
    }
    attached_out->control.pdu_len = (uint8_t)(offset + 1u + 2u * emit);
    return TAVRN_TC_METADATA_OK;
}

static void tc_metadata_clear_request_in(
    tavrn_tc_metadata_slot_t slots[TAVRN_TC_METADATA_CANDIDATE_CAPACITY],
    const tavrn_adva_t *subject)
{
    uint8_t index;

    for (index = 0u; index < TAVRN_TC_METADATA_CANDIDATE_CAPACITY; index++) {
        if (slots[index].valid != 0u &&
            slots[index].candidate.kind == TAVRN_METADATA_SOFT_REQUEST &&
            adva_equal(&slots[index].candidate.subject, subject)) {
            memset(&slots[index], 0, sizeof(slots[index]));
        }
    }
}

static tavrn_tc_metadata_status_t tc_metadata_answer_request(
    const tavrn_tc_metadata_state_t *state, tavrn_gtt_t *gtt,
    tavrn_tc_metadata_slot_t slots[TAVRN_TC_METADATA_CANDIDATE_CAPACITY],
    tavrn_tc_metadata_cooldown_t cooldown[TAVRN_TC_METADATA_CANDIDATE_CAPACITY],
    const tavrn_metadata_candidate_t *request, uint32_t now_ms);

static int tc_metadata_outer_is_departed(const tavrn_tc_metadata_state_t *state,
                                         const tavrn_adva_t *outer,
                                         uint32_t now_ms)
{
    tavrn_gtt_expiry_snapshot_t snapshot;

    if (state->gtt == NULL) {
        return 0;
    }
    if (tavrn_gtt_expiry_snapshot(state->gtt, outer, now_ms, &snapshot) ==
        TAVRN_GTT_EXPIRY_QUERY_FOUND) {
        return snapshot.freshness == TAVRN_GTT_FRESHNESS_DEPARTED;
    }
    return 0;
}

static tavrn_tc_metadata_status_t tc_metadata_merge_answer(
    tavrn_gtt_t *gtt, uint32_t *imported_non_direct_count,
    const tavrn_metadata_candidate_t *answer, uint32_t now_ms)
{
    tavrn_gtt_metadata_merge_status_t merge_status;

    if (imported_non_direct_count == NULL) {
        return TAVRN_TC_METADATA_INVALID;
    }
    if (gtt == NULL) {
        return TAVRN_TC_METADATA_APPLIED;
    }
    merge_status = tavrn_gtt_metadata_merge(gtt, &answer->subject,
                                             answer->ttl_bucket, now_ms);
    if (merge_status == TAVRN_GTT_METADATA_MERGE_COMMITTED ||
        merge_status == TAVRN_GTT_METADATA_MERGE_UNCHANGED) {
        (*imported_non_direct_count)++;
        return TAVRN_TC_METADATA_APPLIED;
    }
    return merge_status == TAVRN_GTT_METADATA_MERGE_TOMBSTONE ?
        TAVRN_TC_METADATA_INVALID : TAVRN_TC_METADATA_BUSY;
}

static tavrn_tc_metadata_status_t tc_metadata_receive_resolved(
    tavrn_tc_metadata_state_t *state, const tavrn_metadata_rx_t *rx,
    uint32_t now_ms, uint8_t answer_requests)
{
    uint8_t capacity;
    uint8_t index;
    uint8_t answer_present = 0u;
    tavrn_tc_metadata_transaction_t *transaction;
    tavrn_gtt_t staged_gtt;
    tavrn_gtt_t *gtt;
    tavrn_gtt_t *staged_gtt_or_null = NULL;

    if (!tc_metadata_state_valid(state) || rx == NULL || rx->metadata_flag != 1u ||
        rx->stale != 0u || !tc_metadata_adva_valid(&rx->outer_transmitter)) {
        return TAVRN_TC_METADATA_INVALID;
    }
    capacity = tc_metadata_frame_capacity(rx->frame_kind, rx->unreachable_count);
    if (capacity == 0xffu || rx->count == 0u || rx->count > capacity) {
        return TAVRN_TC_METADATA_INVALID;
    }
    for (index = 0u; index < rx->count; index++) {
        if (!tc_metadata_candidate_valid(&rx->entries[index])) {
            return TAVRN_TC_METADATA_INVALID;
        }
        if (rx->entries[index].freshness_request == 0u) {
            answer_present = 1u;
        }
    }
    gtt = state->gtt;
    if (gtt != NULL && (gtt->storage == NULL ||
                        (answer_present != 0u &&
                         tc_metadata_outer_is_departed(state,
                                                       &rx->outer_transmitter,
                                                       now_ms)))) {
        return TAVRN_TC_METADATA_INVALID;
    }
    transaction = &state->metadata_transaction;
    memset(transaction, 0, sizeof(*transaction));
    memcpy(transaction->slots, state->slots, sizeof(transaction->slots));
    memcpy(transaction->cooldown, state->cooldown, sizeof(transaction->cooldown));
    transaction->round_robin_cursor = state->round_robin_cursor;
    transaction->imported_non_direct_count = state->imported_non_direct_count;
    if (gtt != NULL) {
        transaction->gtt_storage = *gtt->storage;
        staged_gtt = *gtt;
        staged_gtt.storage = &transaction->gtt_storage;
        staged_gtt_or_null = &staged_gtt;
    }
    /* Validation, outer tombstone admission, and every GTT mutation use the
     * copied transaction.  Nothing escapes if any entry cannot merge. */
    for (index = 0u; index < rx->count; index++) {
        const tavrn_metadata_candidate_t *entry = &rx->entries[index];

        if (entry->freshness_request != 0u) {
            tavrn_tc_metadata_status_t status = answer_requests != 0u ?
                tc_metadata_answer_request(state, staged_gtt_or_null,
                                           transaction->slots, transaction->cooldown,
                                           entry, now_ms) :
                tc_metadata_create_in(state, transaction->slots,
                                      transaction->cooldown, entry, now_ms);
            if (status != TAVRN_TC_METADATA_OK) {
                memset(transaction, 0, sizeof(*transaction));
                return status;
            }
        } else {
            tavrn_tc_metadata_status_t status = tc_metadata_merge_answer(
                staged_gtt_or_null, &transaction->imported_non_direct_count,
                entry, now_ms);

            if (status != TAVRN_TC_METADATA_APPLIED) {
                memset(transaction, 0, sizeof(*transaction));
                return status;
            }
            tc_metadata_clear_request_in(transaction->slots, &entry->subject);
        }
    }
    if (gtt != NULL) {
        *gtt->storage = transaction->gtt_storage;
    }
    memcpy(state->slots, transaction->slots, sizeof(state->slots));
    memcpy(state->cooldown, transaction->cooldown, sizeof(state->cooldown));
    state->round_robin_cursor = transaction->round_robin_cursor;
    state->imported_non_direct_count = transaction->imported_non_direct_count;
    memset(transaction, 0, sizeof(*transaction));
    return TAVRN_TC_METADATA_APPLIED;
}

tavrn_tc_metadata_status_t tavrn_maintenance_metadata_receive(
    tavrn_tc_metadata_state_t *state, const tavrn_metadata_rx_t *rx,
    uint32_t now_ms)
{
    return tc_metadata_receive_resolved(state, rx, now_ms, 0u);
}

static int tc_metadata_resolve_sid8(const tavrn_tc_metadata_state_t *state,
                                    uint8_t sid8, const tavrn_adva_t *outer,
                                    tavrn_adva_t *subject_out)
{
    uint8_t index;
    uint8_t matches = 0u;
    uint8_t outer_matches;

    if (state == NULL || sid8 == 0u || sid8 == 0xffu || subject_out == NULL) return 0;
    outer_matches = outer != NULL && outer->bytes[0] == sid8 &&
        tc_metadata_adva_valid(outer);
    if (state->config.local_identity.bytes[0] == sid8) {
        *subject_out = state->config.local_identity;
        return 1;
    }
    if (state->gtt == NULL || state->gtt->storage == NULL) {
        if (outer_matches != 0u) {
            *subject_out = *outer;
            return 1;
        }
        return 0;
    }
    for (index = 0u; index < TAVRN_GTT_CAPACITY; index++) {
        const tavrn_gtt_entry_t *entry = &state->gtt->storage->entries[index];

        if (entry->occupied != 0u && entry->departed == 0u &&
            entry->identity.bytes[0] == sid8) {
            *subject_out = entry->identity;
            matches++;
        }
    }
    if (matches == 0u && outer_matches != 0u) {
        *subject_out = *outer;
        return 1;
    }
    if (matches == 1u && outer_matches != 0u && !adva_equal(subject_out, outer)) {
        return 0;
    }
    return matches == 1u;
}

static int tc_metadata_control_extension(
    const tavrn_rx_control_event_t *event, tavrn_metadata_frame_kind_t *kind_out,
    uint8_t *capacity_out, uint8_t *offset_out)
{
    const tavrn_validated_control_t *control;
    uint8_t unreachable_count;
    uint8_t metadata_mask;
    uint8_t base_len;
    uint8_t count;

    if (event == NULL || kind_out == NULL || capacity_out == NULL || offset_out == NULL ||
        event->transmitter.logical_id.width != TAVRN_IDENTITY_SID8) {
        return 0;
    }
    control = &event->control;
    if ((control->pdu[5] & 0x80u) == 0u) return 0;
    if (control->type == TAVRN_WIRE_E_RREQ) {
        *kind_out = TAVRN_METADATA_FRAME_RREQ8;
        *capacity_out = 4u;
        base_len = 15u;
        metadata_mask = 0x08u;
    } else if (control->type == TAVRN_WIRE_E_RREP) {
        *kind_out = TAVRN_METADATA_FRAME_RREP8;
        *capacity_out = 3u;
        base_len = 16u;
        metadata_mask = 0x08u;
    } else if (control->type == TAVRN_WIRE_E_RERR) {
        unreachable_count = control->pdu[10];
        *kind_out = TAVRN_METADATA_FRAME_RERR8;
        *capacity_out = tc_metadata_frame_capacity(*kind_out, unreachable_count);
        if (*capacity_out == 0xffu) return 0;
        base_len = (uint8_t)(11u + 3u * unreachable_count);
        metadata_mask = 0x10u;
    } else {
        return 0;
    }
    if ((control->pdu[5] & metadata_mask) == 0u || control->pdu_len <= base_len) {
        return 0;
    }
    count = control->pdu[base_len];
    if (count == 0u || count > *capacity_out ||
        control->pdu_len != (uint8_t)(base_len + 1u + 2u * count)) {
        return 0;
    }
    *offset_out = base_len;
    return 1;
}

static tavrn_tc_metadata_status_t tc_metadata_answer_request(
    const tavrn_tc_metadata_state_t *state, tavrn_gtt_t *gtt,
    tavrn_tc_metadata_slot_t slots[TAVRN_TC_METADATA_CANDIDATE_CAPACITY],
    tavrn_tc_metadata_cooldown_t cooldown[TAVRN_TC_METADATA_CANDIDATE_CAPACITY],
    const tavrn_metadata_candidate_t *request, uint32_t now_ms)
{
    tavrn_metadata_candidate_t answer;
    tavrn_gtt_expiry_snapshot_t snapshot;
    tavrn_gtt_expiry_query_status_t query;
    uint32_t remaining_ms;
    uint32_t bucket;
    tavrn_tc_metadata_status_t status;

    if (request->freshness_request == 0u) return TAVRN_TC_METADATA_INVALID;
    answer = *request;
    answer.freshness_request = 0u;
    if (adva_equal(&request->subject, &state->config.local_identity)) {
        answer.kind = TAVRN_METADATA_SUBJECT_SELF_ANSWER;
        answer.ttl_bucket = 15u;
    } else {
        if (gtt == NULL) return TAVRN_TC_METADATA_OK;
        memset(&snapshot, 0, sizeof(snapshot));
        query = tavrn_gtt_expiry_snapshot(gtt, &request->subject, now_ms,
                                          &snapshot);
        if (query != TAVRN_GTT_EXPIRY_QUERY_FOUND ||
            snapshot.freshness != TAVRN_GTT_FRESHNESS_ACTIVE) {
            return TAVRN_TC_METADATA_OK;
        }
        remaining_ms = snapshot.hard_deadline_ms - now_ms;
        bucket = remaining_ms / 20000u;
        if (bucket > 15u) bucket = 15u;
        if (bucket <= (uint32_t)request->ttl_bucket * 2u) {
            return TAVRN_TC_METADATA_OK;
        }
        answer.kind = TAVRN_METADATA_INTERMEDIARY_ANSWER;
        answer.ttl_bucket = (uint8_t)bucket;
    }
    status = tc_metadata_create_in(state, slots, cooldown, &answer, now_ms);
    /* An answer is opportunistic social metadata.  A live four-slot pool
     * suppresses it rather than displacing any existing request or reservation. */
    return status == TAVRN_TC_METADATA_BUSY ? TAVRN_TC_METADATA_OK : status;
}

tavrn_tc_metadata_status_t tavrn_maintenance_metadata_receive_control(
    tavrn_tc_metadata_state_t *state, const tavrn_rx_control_event_t *event,
    uint32_t now_ms)
{
    tavrn_metadata_rx_t rx;
    tavrn_metadata_frame_kind_t kind;
    uint8_t capacity;
    uint8_t offset;
    uint8_t index;

    if (!tc_metadata_state_valid(state) || event == NULL ||
        event->transmitter.logical_id.width != TAVRN_IDENTITY_SID8 ||
        !tc_metadata_adva_valid(&event->transmitter.adva) ||
        !tc_metadata_control_extension(event, &kind, &capacity, &offset)) {
        return TAVRN_TC_METADATA_INVALID;
    }
    memset(&rx, 0, sizeof(rx));
    rx.frame_kind = kind;
    rx.outer_transmitter = event->transmitter.adva;
    rx.sid8 = event->transmitter.logical_id.value;
    rx.metadata_flag = 1u;
    rx.count = event->control.pdu[offset];
    rx.unreachable_count = kind == TAVRN_METADATA_FRAME_RERR8 ?
        event->control.pdu[10] : 0u;
    if (rx.count > capacity) return TAVRN_TC_METADATA_INVALID;
    for (index = 0u; index < rx.count; index++) {
        tavrn_metadata_candidate_t *candidate = &rx.entries[index];
        uint8_t entry_offset = (uint8_t)(offset + 1u + 2u * index);
        uint8_t ttl_flags = event->control.pdu[(uint8_t)(entry_offset + 1u)];

        if ((ttl_flags & 0x0cu) != 0u || (ttl_flags & 0x02u) != 0u ||
            !tc_metadata_resolve_sid8(state, event->control.pdu[entry_offset],
                                       &event->transmitter.adva, &candidate->subject)) {
            return TAVRN_TC_METADATA_INVALID;
        }
        candidate->subject_sid8 = event->control.pdu[entry_offset];
        candidate->ttl_bucket = (uint8_t)(ttl_flags >> 4);
        candidate->freshness_request = ttl_flags & 0x01u;
        candidate->kind = candidate->freshness_request != 0u ?
            TAVRN_METADATA_SOFT_REQUEST : TAVRN_METADATA_SUBJECT_SELF_ANSWER;
    }
    {
        tavrn_tc_metadata_status_t status = tc_metadata_receive_resolved(
            state, &rx, now_ms, 1u);

        /* Isolated API fixtures have no GTT transaction to expose, but retain
         * their historic one-frame imported-observation accounting. */
        if (status == TAVRN_TC_METADATA_APPLIED && state->gtt == NULL) {
            state->imported_non_direct_count++;
        }
        return status;
    }
}

tavrn_tc_metadata_status_t tavrn_maintenance_tc_metadata_snapshot(
    const tavrn_tc_metadata_state_t *state,
    tavrn_tc_metadata_snapshot_t *snapshot_out)
{
    uint8_t index;

    if (snapshot_out != NULL) {
        memset(snapshot_out, 0, sizeof(*snapshot_out));
    }
    if (!tc_metadata_state_valid(state) || snapshot_out == NULL) {
        return TAVRN_TC_METADATA_INVALID;
    }
    snapshot_out->next_tc_sequence = state->next_tc_sequence;
    snapshot_out->round_robin_cursor = state->round_robin_cursor;
    snapshot_out->local_apply_count = state->local_apply_count;
    snapshot_out->relay_count = state->relay_count;
    snapshot_out->imported_non_direct_count = state->imported_non_direct_count;
    for (index = 0u; index < TAVRN_TC_METADATA_UUID_CAPACITY; index++) {
        snapshot_out->uuid_entries += state->uuid[index].valid != 0u ? 1u : 0u;
    }
    for (index = 0u; index < TAVRN_TC_METADATA_SUBJECT_CAPACITY; index++) {
        snapshot_out->subject_entries += state->subject[index].valid != 0u ? 1u : 0u;
    }
    for (index = 0u; index < TAVRN_TC_METADATA_CANDIDATE_CAPACITY; index++) {
        snapshot_out->candidate_count += state->slots[index].valid != 0u ? 1u : 0u;
        snapshot_out->cooldown_count += state->cooldown[index].valid != 0u ? 1u : 0u;
    }
    return TAVRN_TC_METADATA_OK;
}

tavrn_metadata_reservation_handle_t tavrn_maintenance_metadata_reserve_delayed(
    tavrn_maintenance_t *maintenance, const tavrn_adva_t *subject,
    uint8_t context_index, uint32_t now_ms)
{
    tavrn_metadata_candidate_t candidate;
    uint8_t index;
    int free_slot = -1;

    (void)now_ms;
    if (!maintenance_is_initialized(maintenance) || !tc_metadata_adva_valid(subject)) {
        return 0u;
    }
    for (index = 0u; index < TAVRN_TC_METADATA_CANDIDATE_CAPACITY; index++) {
        tavrn_tc_metadata_slot_t *slot = &maintenance->tc_metadata.slots[index];

        if (slot->valid != 0u && slot->candidate.kind ==
                TAVRN_METADATA_DELAYED_TARGETED_RESERVATION &&
            slot->targeted_context_index == context_index) {
            return (tavrn_metadata_reservation_handle_t)
                (slot->delayed_generation << TAVRN_METADATA_RESERVATION_GENERATION_SHIFT |
                 (uint32_t)context_index << TAVRN_METADATA_RESERVATION_CONTEXT_SHIFT |
                 index);
        }
        if (slot->valid == 0u && free_slot < 0) {
            free_slot = (int)index;
        }
    }
    if (free_slot < 0) {
        return 0u;
    }
    memset(&candidate, 0, sizeof(candidate));
    candidate.subject = *subject;
    candidate.subject_sid8 = subject->bytes[0];
    candidate.kind = TAVRN_METADATA_DELAYED_TARGETED_RESERVATION;
    index = (uint8_t)free_slot;
    maintenance->tc_metadata.delayed_generation[index]++;
    if (maintenance->tc_metadata.delayed_generation[index] == 0u ||
        maintenance->tc_metadata.delayed_generation[index] >
            TAVRN_METADATA_RESERVATION_GENERATION_MAX) {
        maintenance->tc_metadata.delayed_generation[index] = 1u;
    }
    maintenance->tc_metadata.slots[index].candidate = candidate;
    maintenance->tc_metadata.slots[index].targeted_context_index = context_index;
    maintenance->tc_metadata.slots[index].delayed_generation =
        maintenance->tc_metadata.delayed_generation[index];
    maintenance->tc_metadata.slots[index].valid = 1u;
    return (tavrn_metadata_reservation_handle_t)
        (maintenance->tc_metadata.slots[index].delayed_generation <<
             TAVRN_METADATA_RESERVATION_GENERATION_SHIFT |
         (uint32_t)context_index << TAVRN_METADATA_RESERVATION_CONTEXT_SHIFT | index);
}

void tavrn_maintenance_metadata_release_delayed(
    tavrn_maintenance_t *maintenance, tavrn_metadata_reservation_handle_t handle)
{
    uint8_t slot_index = (uint8_t)(handle & TAVRN_METADATA_RESERVATION_SLOT_MASK);
    uint8_t context_index = (uint8_t)(handle >>
        TAVRN_METADATA_RESERVATION_CONTEXT_SHIFT);
    uint32_t generation = handle >> TAVRN_METADATA_RESERVATION_GENERATION_SHIFT;
    tavrn_tc_metadata_slot_t *slot;

    if (!maintenance_is_initialized(maintenance) || handle == 0u ||
        generation == 0u || slot_index >= TAVRN_TC_METADATA_CANDIDATE_CAPACITY) {
        return;
    }
    slot = &maintenance->tc_metadata.slots[slot_index];
    if (slot->valid != 0u && slot->candidate.kind ==
            TAVRN_METADATA_DELAYED_TARGETED_RESERVATION &&
        slot->targeted_context_index == context_index &&
        slot->delayed_generation == generation) {
        memset(slot, 0, sizeof(*slot));
    }
}

tavrn_tc_metadata_status_t tavrn_maintenance_tc_on_verified_departure(
    tavrn_maintenance_t *maintenance, const tavrn_adva_t *subject, uint32_t now_ms)
{
    tavrn_tc_metadata_status_t status;

    if (!maintenance_is_initialized(maintenance) || !tc_metadata_adva_valid(subject)) {
        return TAVRN_TC_METADATA_INVALID;
    }
    status = tc_metadata_enqueue_origin_fact(&maintenance->tc_metadata, subject,
                                             TAVRN_TC_EVENT_LEAVE);
    if (status == TAVRN_TC_METADATA_PREPARED || status == TAVRN_TC_METADATA_RETAINED) {
        tc_metadata_preempt_rfi_for_local_fact(maintenance, now_ms);
    }
    return status;
}

tavrn_tc_metadata_status_t tavrn_maintenance_tc_on_direct_timeout_departure(
    tavrn_maintenance_t *maintenance, const tavrn_adva_t *subject, uint32_t now_ms)
{
    tavrn_tc_metadata_status_t status;

    if (!maintenance_is_initialized(maintenance) || !tc_metadata_adva_valid(subject)) {
        return TAVRN_TC_METADATA_INVALID;
    }
    status = tc_metadata_enqueue_origin_fact(&maintenance->tc_metadata, subject,
                                             TAVRN_TC_EVENT_LEAVE);
    if (status == TAVRN_TC_METADATA_PREPARED || status == TAVRN_TC_METADATA_RETAINED) {
        tc_metadata_preempt_rfi_for_local_fact(maintenance, now_ms);
    }
    return status;
}

tavrn_tc_metadata_status_t tavrn_maintenance_tc_on_retry_exhausted(
    tavrn_maintenance_t *maintenance, const tavrn_adva_t *failed_next_hop,
    const tavrn_adva_t *final_destination_or_null, uint32_t now_ms)
{
    tavrn_tc_metadata_status_t status;

    (void)final_destination_or_null;
    if (!maintenance_is_initialized(maintenance) ||
        !tc_metadata_adva_valid(failed_next_hop)) {
        return TAVRN_TC_METADATA_INVALID;
    }
    status = tc_metadata_enqueue_retry_exhausted_leave(&maintenance->tc_metadata,
                                                        failed_next_hop);
    if (status == TAVRN_TC_METADATA_PREPARED || status == TAVRN_TC_METADATA_RETAINED) {
        tc_metadata_preempt_rfi_for_local_fact(maintenance, now_ms);
    }
    return status;
}

static tavrn_tc_admission_t tc_metadata_link_admission(tavrn_link_send_status_t status)
{
    if (status == TAVRN_LINK_SEND_OK) return TAVRN_TC_ADMISSION_ADMITTED;
    if (status == TAVRN_LINK_SEND_BUSY || status == TAVRN_LINK_SEND_NO_SLOT) {
        return TAVRN_TC_ADMISSION_BUSY;
    }
    if (status == TAVRN_LINK_SEND_LOCAL_NOT_ATTEMPTED) {
        return TAVRN_TC_ADMISSION_LOCAL_NOT_ATTEMPTED;
    }
    return TAVRN_TC_ADMISSION_REJECTED;
}

tavrn_tc_metadata_status_t tavrn_maintenance_metadata_owner_tick(
    tavrn_maintenance_t *maintenance, uint32_t now_ms)
{
    tavrn_metadata_pending_transaction_t *pending;
    tavrn_router_event_status_t status;
    const tavrn_direct_peer_t *next_hop;

    if (!maintenance_is_initialized(maintenance)) {
        return TAVRN_TC_METADATA_INVALID;
    }
    pending = &maintenance->metadata_pending;
    if (pending->valid == 0u) {
        return TAVRN_TC_METADATA_OK;
    }
    if (pending->terminal_failure != 0u) {
        return TAVRN_TC_METADATA_INVALID;
    }
    if (pending->scheduler_token != BLE_MESH_TX_TOKEN_NONE &&
        tavrn_link_v2_tracked_token_in_use(maintenance->router->link,
                                            pending->scheduler_token) == 0) {
        /* A higher-priority queue admission can evict a completion-tracked
         * best-effort control before the scheduler emits its terminal event.
         * Loss of token ownership is therefore the same retriable no-physical-
         * completion result as a zero-channel TX_FAILED. */
        pending->scheduler_token = BLE_MESH_TX_TOKEN_NONE;
        pending->retry_pending = 1u;
        maintenance->metadata_completion_retry_count++;
    }
    if (pending->scheduler_token != BLE_MESH_TX_TOKEN_NONE ||
        pending->retry_pending == 0u) {
        return TAVRN_TC_METADATA_RETAINED;
    }
    next_hop = pending->controlled_flood != 0u ? NULL : &pending->next_hop;
    status = tavrn_router_retry_retained_control(
        maintenance->router, &pending->base, &pending->attached.control, next_hop,
        pending->controlled_flood, now_ms);
    if (status == TAVRN_ROUTER_EVENT_INVALID) {
        return TAVRN_TC_METADATA_INVALID;
    }
    return status == TAVRN_ROUTER_EVENT_BUSY ? TAVRN_TC_METADATA_RETAINED :
                                               TAVRN_TC_METADATA_OK;
}

tavrn_tc_metadata_status_t tavrn_maintenance_tc_owner_tick(
    tavrn_maintenance_t *maintenance, uint32_t now_ms)
{
    tavrn_tc_metadata_state_t *state;
    tavrn_tc_metadata_action_t *pending;
    tavrn_tc_metadata_action_t action;
    tavrn_validated_control_t control;
    tavrn_link_event_t outcome;
    tavrn_link_send_status_t send_status;
    tavrn_tc_admission_t admission;
    tavrn_tc_metadata_status_t commit_status;
    tavrn_tc_metadata_status_t promote_status;
    tavrn_router_event_status_t outcome_status = TAVRN_ROUTER_EVENT_IGNORED;
    uint8_t origin;

    if (!maintenance_is_initialized(maintenance) || !tc_metadata_state_valid(
            &maintenance->tc_metadata)) {
        return TAVRN_TC_METADATA_INVALID;
    }
    state = &maintenance->tc_metadata;
    promote_status = tc_metadata_promote_origin(state);
    if (promote_status == TAVRN_TC_METADATA_INVALID) {
        return promote_status;
    }
    pending = state->origin_pending.valid != 0u ? &state->origin_pending :
        &state->relay_pending;
    origin = state->origin_pending.valid != 0u;
    if (pending->valid == 0u) return TAVRN_TC_METADATA_OK;
    if (origin != 0u) {
        /* The local FIFO remains a real TC obligation during every admission
         * retry.  This also covers a retained action constructed before the
         * binding was installed. */
        tc_metadata_preempt_rfi_for_local_fact(maintenance, now_ms);
    }
    action = *pending;
    memset(&control, 0, sizeof(control));
    control.type = TAVRN_WIRE_TC_UPDATE;
    control.pdu_len = TAVRN_LINK_CONTROL_PDU_MAX;
    memcpy(control.pdu, action.pdu, sizeof(control.pdu));
    memset(&outcome, 0, sizeof(outcome));
    send_status = tavrn_link_v2_send_control(maintenance->router->link,
                                              &control,
                                             NULL, 1u, now_ms, &outcome);
    admission = tc_metadata_link_admission(send_status);
    commit_status = origin != 0u ? tavrn_maintenance_tc_commit_origin(
        state, &action, admission, now_ms) : tavrn_maintenance_tc_commit_relay(
        state, &action, admission, now_ms);
    if (commit_status == TAVRN_TC_METADATA_OK) {
        if (origin != 0u) {
            tc_metadata_commit_origin_fact(state, &action);
            /* Promotion occurs only after the admitted FIFO head opened a
             * physical slot.  A BUSY send therefore cannot let the overflow
             * overtake or suppress that older head. */
            tc_metadata_promote_retry_exhausted_leave_overflow(state);
        }
        tavrn_router_note_local_broadcast(maintenance->router, now_ms);
    }
    /* TC admission can evict a queued DATA custody item even though the TC
     * itself was accepted.  Consume that exact ownership transfer once after
     * committing the admitted TC state; an unreleasable DATA is fail-stop. */
    if (outcome.type != TAVRN_LINK_EVENT_NONE) {
        outcome_status = tavrn_router_handle_link_event(maintenance->router, &outcome,
                                                         now_ms);
        if (outcome_status == TAVRN_ROUTER_EVENT_INVALID) {
            return TAVRN_TC_METADATA_INVALID;
        }
    }
    return commit_status;
}

tavrn_tc_metadata_status_t tavrn_maintenance_tc_metadata_telemetry(
    const tavrn_maintenance_t *maintenance,
    tavrn_tc_metadata_telemetry_t *telemetry_out)
{
    const tavrn_tc_metadata_state_t *state;
    const tavrn_tc_metadata_action_t *current;

    if (telemetry_out != NULL) {
        memset(telemetry_out, 0, sizeof(*telemetry_out));
    }
    if (!maintenance_is_initialized(maintenance) || telemetry_out == NULL ||
        !tc_metadata_state_valid(&maintenance->tc_metadata)) {
        return TAVRN_TC_METADATA_INVALID;
    }
    state = &maintenance->tc_metadata;
    current = state->origin_pending.valid != 0u ? &state->origin_pending :
        &state->relay_pending;
    telemetry_out->tc_origin_busy_count = state->origin_busy_count;
    telemetry_out->tc_relay_busy_count = state->relay_busy_count;
    telemetry_out->tc_apply_count = state->local_apply_count;
    telemetry_out->tc_duplicate_count = state->duplicate_count;
    telemetry_out->tc_relay_count = state->relay_count;
    telemetry_out->tc_admission_count = state->admission_count;
    telemetry_out->metadata_completion_commit_count =
        maintenance->metadata_completion_commit_count;
    telemetry_out->metadata_completion_retry_count =
        maintenance->metadata_completion_retry_count;
    telemetry_out->metadata_completion_failure_count =
        maintenance->metadata_completion_failure_count;
    telemetry_out->last_tc_sequence = state->last_tc_sequence;
    telemetry_out->last_tc_event = state->last_tc_event;
    telemetry_out->last_subject_sid8 = state->last_subject_sid8;
    telemetry_out->origin_queue_depth = (uint8_t)(state->origin_fact_count +
        (state->retry_exhausted_leave_overflow.valid != 0u ? 1u : 0u));
    telemetry_out->relay_queue_depth = state->relay_queue_count;
    telemetry_out->metadata_base_type = maintenance->metadata_last_base_type;
    telemetry_out->metadata_emitted_count = maintenance->metadata_last_emitted_count;
    telemetry_out->metadata_pdu_len = maintenance->metadata_last_pdu_len;
    telemetry_out->metadata_transaction_active = maintenance->metadata_pending.valid;
    if (current->valid != 0u) {
        telemetry_out->current_tc_sequence = current->tc_sequence;
        telemetry_out->current_tc_event = current->event;
        telemetry_out->current_subject_sid8 = current->subject.bytes[0];
    }
    if (maintenance->metadata_pending.valid != 0u) {
        telemetry_out->metadata_base_type = maintenance->metadata_pending.attached.control.type;
        telemetry_out->metadata_emitted_count =
            maintenance->metadata_pending.attached.emitted.count;
        telemetry_out->metadata_pdu_len = maintenance->metadata_pending.attached.control.pdu_len;
    }
    return TAVRN_TC_METADATA_OK;
}
