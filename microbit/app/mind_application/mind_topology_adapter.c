#include "mind_topology_adapter.h"

#include <limits.h>
#include <string.h>

static void increment_saturating(uint32_t *value)
{
    if (value != NULL && *value != UINT32_MAX) {
        (*value)++;
    }
}

void mind_topology_adapter_init(mind_topology_adapter_t *adapter)
{
    if (adapter != NULL) {
        memset(adapter, 0, sizeof(*adapter));
    }
}

void mind_topology_adapter_note_snapshot_failure(mind_topology_adapter_t *adapter)
{
    if (adapter != NULL) {
        increment_saturating(&adapter->counters.snapshots_failed);
    }
}

int mind_topology_adapter_copy_snapshot(
    mind_topology_adapter_t *adapter,
    const routed_cycle_gtt_snapshot_t *snapshot)
{
    uint8_t index;

    if (adapter == NULL || snapshot == NULL ||
        snapshot->entry_count > MIND_APP_TOPOLOGY_CAPACITY) {
        return 0;
    }

    memset(adapter->entries, 0, sizeof(adapter->entries));
    for (index = 0u; index < snapshot->entry_count; index++) {
        const routed_cycle_gtt_snapshot_entry_t *source = &snapshot->entries[index];
        mind_topology_entry_t *destination = &adapter->entries[index];

        destination->canonical_adva = source->canonical_adva;
        destination->last_evidence_ms = source->last_evidence_ms;
        destination->soft_deadline_ms = source->soft_deadline_ms;
        destination->hard_deadline_ms = source->hard_deadline_ms;
        destination->departed_deadline_ms = source->departed_deadline_ms;
        destination->serial = source->serial;
        destination->hop_count = source->hop_count;
        destination->serial_state = source->serial_state;
        destination->hop_state = source->hop_state;
        destination->freshness = source->freshness;
        destination->departed = source->departed;
    }
    adapter->query_at_ms = snapshot->query_at_ms;
    adapter->entry_count = snapshot->entry_count;
    adapter->valid = 1u;
    increment_saturating(&adapter->counters.snapshots_ok);
    if (UINT32_MAX - adapter->counters.copied_entries < snapshot->entry_count) {
        adapter->counters.copied_entries = UINT32_MAX;
    } else {
        adapter->counters.copied_entries += snapshot->entry_count;
    }
    return 1;
}
