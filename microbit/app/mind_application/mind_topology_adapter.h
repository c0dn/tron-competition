#ifndef MIND_TOPOLOGY_ADAPTER_H
#define MIND_TOPOLOGY_ADAPTER_H

/* Read-only Layer-7 copy of the routed FULL telemetry projection.  This is
 * deliberately a consumer of copied GTT facts, never a second GTT owner. */

#include <stdint.h>

#include "routed_cycle.h"

#define MIND_APP_TOPOLOGY_CAPACITY 16u

typedef struct mind_topology_entry {
    tavrn_adva_t canonical_adva;
    uint32_t last_evidence_ms;
    uint32_t soft_deadline_ms;
    uint32_t hard_deadline_ms;
    uint32_t departed_deadline_ms;
    uint16_t serial;
    uint8_t hop_count;
    routed_cycle_value_state_t serial_state;
    routed_cycle_value_state_t hop_state;
    routed_cycle_gtt_freshness_t freshness;
    routed_cycle_boolean_t departed;
} mind_topology_entry_t;

typedef struct mind_topology_adapter_counters {
    uint32_t snapshots_ok;
    uint32_t snapshots_failed;
    uint32_t copied_entries;
} mind_topology_adapter_counters_t;

typedef struct mind_topology_adapter {
    mind_topology_entry_t entries[MIND_APP_TOPOLOGY_CAPACITY];
    mind_topology_adapter_counters_t counters;
    uint32_t query_at_ms;
    uint8_t entry_count;
    uint8_t valid;
} mind_topology_adapter_t;

void mind_topology_adapter_init(mind_topology_adapter_t *adapter);

/* A non-OK frozen snapshot is represented by this explicit observation.  It
 * preserves the previous copy verbatim and never claims a truncation fact. */
void mind_topology_adapter_note_snapshot_failure(mind_topology_adapter_t *adapter);

/* Copy an already-OK routed snapshot into Layer-7 owned storage. */
int mind_topology_adapter_copy_snapshot(
    mind_topology_adapter_t *adapter,
    const routed_cycle_gtt_snapshot_t *snapshot);

#endif /* MIND_TOPOLOGY_ADAPTER_H */
