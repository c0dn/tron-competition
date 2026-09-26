#ifndef TAVRN_PHASE4_INCARNATION_CONTRACT_H
#define TAVRN_PHASE4_INCARNATION_CONTRACT_H

/*
 * Frozen routed-common incarnation API for the Step 5.6 implementation.
 *
 * tavrn_router.h must define TAVRN_ROUTER_INCARNATION_API and these exact
 * declarations when production support lands.  Until then, the RED-only
 * fallback below lets this contract compile against the established router
 * while tavrn_phase4_incarnation_red_backend.c deliberately supplies no
 * incarnation behavior.
 */
#include "tavrn_router.h"

#ifndef TAVRN_ROUTER_INCARNATION_API

#define TAVRN_ROUTER_INCARNATION_PENDING_RESET_CAPACITY 1u
#define TAVRN_ROUTER_EVENT_REJOINING ((tavrn_router_event_status_t)4)

typedef enum tavrn_router_feature_level {
    TAVRN_ROUTER_FEATURE_AODV_ONLY = 0,
    TAVRN_ROUTER_FEATURE_FULL_TAVRN,
} tavrn_router_feature_level_t;

typedef enum tavrn_router_incarnation_state {
    TAVRN_ROUTER_INCARNATION_REJOINING = 0,
    TAVRN_ROUTER_INCARNATION_ESTABLISHED,
} tavrn_router_incarnation_state_t;

typedef enum tavrn_router_incarnation_status {
    TAVRN_ROUTER_INCARNATION_OK = 0,
    TAVRN_ROUTER_INCARNATION_BUSY,
    TAVRN_ROUTER_INCARNATION_INVALID,
    TAVRN_ROUTER_INCARNATION_INVALID_CONFIG,
    TAVRN_ROUTER_INCARNATION_NOT_FOUND,
} tavrn_router_incarnation_status_t;

typedef struct tavrn_router_incarnation_config {
    tavrn_router_feature_level_t feature_level;
    uint16_t boot_nonce;
    uint32_t reboot_announce_ms;
} tavrn_router_incarnation_config_t;

/* This copied host-test snapshot has no mutation authority.  The TX token is
 * the exact scheduler token for latest_hello; TX_DONE only counts when it
 * matches this bootstrap announcement. */
typedef struct tavrn_router_incarnation_snapshot {
    tavrn_router_incarnation_state_t state;
    tavrn_validated_control_t latest_hello;
    ble_mesh_tx_token_t latest_hello_tx_token;
    uint16_t boot_nonce;
    uint32_t last_bootstrap_tx_done_ms;
    uint8_t bootstrap_tx_done_seen;
    uint8_t normal_hello_emitted;
} tavrn_router_incarnation_snapshot_t;

/* Peer keys are full AdvA values.  Counts are copied observations for this
 * peer only; they make the atomic-clear obligation observable without opening
 * a second route engine or mutable test backdoor. */
typedef struct tavrn_router_incarnation_peer_snapshot {
    tavrn_direct_peer_t direct_peer;
    uint16_t boot_nonce;
    uint8_t direct_binding_valid;
    uint8_t route_barred;
    uint8_t reset_pending;
    uint8_t pending_custody_count;
    uint8_t data_dedupe_count;
    uint8_t control_dedupe_count;
    uint8_t freshness_count;
    uint8_t route_count;
} tavrn_router_incarnation_peer_snapshot_t;

typedef struct tavrn_router_incarnation_counters {
    uint32_t direct_bootstrap_admitted;
    uint32_t same_tuple_idempotent;
    uint32_t reset_committed;
    uint32_t reset_busy;
    uint32_t reset_overflow;
    uint32_t invalid_bootstrap_rejected;
} tavrn_router_incarnation_counters_t;

/* Platform startup supplies the already-bounded hardware nonce here.  Zero
 * and invalid feature/timer configuration fail closed before router use. */
tavrn_router_incarnation_status_t tavrn_router_init_with_incarnation(
    tavrn_router_t *router, tavrn_link_v2_t *link, aodv_core_t *aodv,
    const tavrn_router_augmentation_hooks_t *augmentation_or_null,
    const tavrn_router_incarnation_config_t *config, uint32_t now_ms);

tavrn_router_incarnation_status_t tavrn_router_incarnation_snapshot(
    const tavrn_router_t *router,
    tavrn_router_incarnation_snapshot_t *snapshot_out);
tavrn_router_incarnation_status_t tavrn_router_incarnation_peer_snapshot(
    const tavrn_router_t *router, const tavrn_adva_t *peer_adva,
    tavrn_router_incarnation_peer_snapshot_t *snapshot_out);
const tavrn_router_incarnation_counters_t *tavrn_router_incarnation_counters(
    const tavrn_router_t *router);

#endif /* TAVRN_ROUTER_INCARNATION_API */

#endif /* TAVRN_PHASE4_INCARNATION_CONTRACT_H */
