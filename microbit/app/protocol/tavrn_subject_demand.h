#ifndef TAVRN_SUBJECT_DEMAND_H
#define TAVRN_SUBJECT_DEMAND_H

#include <stdint.h>

#include "tavrn_wire_v2.h"

/* This header deliberately remains GTT/FULL-free: route, link, and router
 * expose only copied facts about one logical subject plus its canonical AdvA. */
#define TAVRN_SUBJECT_DEMAND_API 1

enum {
    TAVRN_MAINT_DEMAND_PENDING_DATA = 1u << 0,
    TAVRN_MAINT_DEMAND_QUEUED_DATA = 1u << 1,
    TAVRN_MAINT_DEMAND_VALID_ROUTE_TO_SUBJECT = 1u << 2,
    TAVRN_MAINT_DEMAND_VALID_ROUTE_VIA_SUBJECT = 1u << 3,
    TAVRN_MAINT_DEMAND_PRECURSOR = 1u << 4,
    TAVRN_MAINT_DEMAND_DEFERRED_REPAIR = 1u << 5,
    TAVRN_MAINT_DEMAND_CUSTODY_FINAL = 1u << 6,
    TAVRN_MAINT_DEMAND_CUSTODY_NEXT_HOP = 1u << 7,
    TAVRN_MAINT_DEMAND_PENDING_INGEST = 1u << 8,
    TAVRN_MAINT_DEMAND_RETAINED_FINAL = 1u << 9,
    TAVRN_MAINT_DEMAND_RETAINED_NEXT_HOP = 1u << 10,
    TAVRN_MAINT_DEMAND_APPLICATION_REQUEST = 1u << 11,
    /* A maintenance-owned failed-hop verification episode is an internal,
     * retained liveness demand.  It is intentionally distinct from an
     * application request and remains set while scheduler/AODV admission is
     * backpressured. */
    TAVRN_MAINT_DEMAND_FAILED_HOP_VERIFICATION = 1u << 12,
};

typedef struct tavrn_aodv_subject_demand_snapshot {
    uint16_t reason_mask;
    uint8_t valid_route_to_subject;
    uint8_t snapshot_available;
} tavrn_aodv_subject_demand_snapshot_t;

typedef struct tavrn_link_subject_demand_snapshot {
    uint16_t reason_mask;
    uint8_t snapshot_available;
} tavrn_link_subject_demand_snapshot_t;

typedef struct tavrn_router_subject_demand_snapshot {
    uint16_t reason_mask;
    uint8_t snapshot_available;
} tavrn_router_subject_demand_snapshot_t;

#endif /* TAVRN_SUBJECT_DEMAND_H */
