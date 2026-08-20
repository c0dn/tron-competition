import { describe, expect, it } from 'vitest';
import { eventRecord, eventsResponse, healthDevice, healthResponse, rootStatus } from '../test/fixtures';
import { EVIDENCE_LIMIT, FEED_LIMIT, PROCESSED_CURSOR_LIMIT, bridgeStatus, initialState, reducer } from './store';

function receive(events = eventsResponse().events, at = 10_000, epoch = 0) {
  return (state = initialState) => reducer(state, {
    type: 'eventsReceived',
    page: eventsResponse(events),
    receivedAt: at,
    epoch,
  });
}

describe('dashboard event store', () => {
  it('globally deduplicates reports across roots, observers, and reconnect cursor overlap', () => {
    const first = eventRecord({ cursor: 10, root: '1842de524add', observer: 'dc4b0a0603f8' });
    const secondRoot = eventRecord({ cursor: 11, device: 1, root: '1842de524aee', observer: 'dc4b0a0603f9', path: 'local' });
    let state = receive([first])();
    state = receive([secondRoot])(state);
    state = receive([first], 11_000)(state);

    expect(state.feedKeys).toHaveLength(1);
    expect(state.logicalEvents['7:00002a'].evidence).toHaveLength(2);
    expect(state.wearables[7].record.cursor).toBe(11);
  });

  it('saturates evidence truthfully, counts fresh conflicts, and ignores replayed cursors', () => {
    const reports = Array.from({ length: EVIDENCE_LIMIT + 2 }, (_, index) => eventRecord({
      cursor: index + 1,
      device: index,
      root: `${index.toString(16).padStart(12, '0')}`,
      observer: `${(index + 20).toString(16).padStart(12, '0')}`,
    }));
    let state = receive(reports)();
    const logical = state.logicalEvents['7:00002a'];
    expect(logical.evidence).toHaveLength(EVIDENCE_LIMIT);
    expect(logical.evidenceSaturated).toBe(true);

    state = receive([reports[reports.length - 1]])(state);
    expect(state.logicalEvents['7:00002a'].evidence).toHaveLength(EVIDENCE_LIMIT);
    expect(state.logicalEvents['7:00002a'].evidenceSaturated).toBe(true);

    state = receive([eventRecord({ cursor: 20, root: '1842de524aff', observer: 'dc4b0a0603f9' })])(state);
    expect(state.logicalEvents['7:00002a'].evidence).toHaveLength(EVIDENCE_LIMIT);
    expect(state.logicalEvents['7:00002a'].evidenceSaturated).toBe(true);

    state = receive([
      eventRecord({ cursor: 30, confidence: 76 }),
      eventRecord({ cursor: 31, packet: '00002b' }),
      eventRecord({ cursor: 32, wearable: 8 }),
    ])(state);
    expect(state.logicalEvents['7:00002a'].conflict).toBe(true);
    expect(state.conflictCount).toBe(1);
    expect(state.feedKeys).toHaveLength(3);

    const replay = eventRecord({ cursor: 30, confidence: 76 });
    state = receive([replay])(state);
    expect(state.conflictCount).toBe(1);
    expect(state.logicalEvents['7:00002a'].evidenceSaturated).toBe(true);

    state = receive([eventRecord({ cursor: 33, confidence: 77, root: '1842de524aff', observer: 'dc4b0a0603f9' })])(state);
    expect(state.conflictCount).toBe(2);
  });

  it('bounds active state and cursor ledger after 756 events while minimal tombstones rehydrate conflicts', () => {
    const initial = eventRecord({ cursor: 1, wearable: 7, packet: '000001', seq: 1, confidence: 75 });
    const initialConflict = eventRecord({ cursor: 2, wearable: 7, packet: '000001', seq: 1, confidence: 76 });
    const records = Array.from({ length: 756 }, (_, index) => {
      const value = index + 1_000;
      return eventRecord({
        cursor: index + 3,
        wearable: (index % 254) + 1,
        packet: value.toString(16).padStart(6, '0'),
        seq: value & 0xff,
      });
    });
    let state = receive([initial, initialConflict, ...records])();
    expect(state.feedKeys).toHaveLength(FEED_LIMIT);
    expect(Object.keys(state.logicalEvents)).toHaveLength(FEED_LIMIT);
    expect(state.processedCursors).toHaveLength(PROCESSED_CURSOR_LIMIT);
    expect(state.feedKeys).not.toContain('7:000001');
    expect(state.logicalEvents['7:000001']).toBeUndefined();
    expect(state.tombstones['7:000001']).toEqual({ fingerprint: '1:3:75:2400:86:1', conflict: true });

    state = receive([eventRecord({ cursor: 759, wearable: 7, packet: '000001', seq: 1, confidence: 75 })])(state);
    expect(state.feedKeys[0]).toBe('7:000001');
    expect(Object.keys(state.logicalEvents)).toHaveLength(FEED_LIMIT);
    expect(state.logicalEvents['7:000001']).toMatchObject({ conflict: true, evidenceSaturated: false });
    expect(state.logicalEvents['7:000001'].evidence).toHaveLength(1);
    expect(state.processedCursors).toHaveLength(PROCESSED_CURSOR_LIMIT);
  });

  it('marks a cursor gap while continuing from the records supplied by the bridge', () => {
    const page = eventsResponse([eventRecord({ cursor: 40 })], true);
    const state = reducer(initialState, { type: 'eventsReceived', page, receivedAt: 10_000, epoch: 0 });
    expect(state.lastGap).toEqual({ oldest_cursor: 40, current_cursor: 40 });
    expect(state.eventCursor).toBe(40);
    expect(state.feedKeys).toEqual(['7:00002a']);
  });

  it('clears only epoch-local records and lets lower post-reset cursors update wearable state', () => {
    const root = {
      cursor: 5,
      kind: 'root' as const,
      device: 0,
      now: 100,
      local: '1842de524add',
      node: 1,
      role: 'root' as const,
      roots: 1,
      announced: 1,
      acked: 1,
      rejected: 0,
      pending: 0,
      rootless_drop: 0,
    };
    const command = {
      cursor: 6,
      kind: 'command' as const,
      device: 0,
      now: 101,
      local: '1842de524add',
      command: 'on' as const,
      status: 'accepted' as const,
    };
    let state = receive([root, command, eventRecord({ cursor: 10 })])();
    state = reducer(state, { type: 'epochReset' });
    expect(state.epoch).toBe(1);
    expect(state.rootRecords).toEqual({});
    expect(state.commandRecords).toEqual({});
    expect(state.processedCursors).toEqual([]);
    expect(state.logicalEvents['7:00002a']).toBeDefined();

    state = receive([eventRecord({ cursor: 1, wearable: 8, packet: '000001', seq: 1 })], 11_000, 1)(state);
    expect(state.wearables[8]).toMatchObject({ epoch: 1, record: { cursor: 1 } });
    state = receive([eventRecord({ cursor: 2, packet: '00002a', confidence: 76 })], 12_000, 1)(state);
    expect(state.wearables[7]).toMatchObject({ epoch: 1, record: { cursor: 2 } });
  });

  it('tags health snapshots by epoch so stale health cannot override a new-epoch root', () => {
    let state = reducer(initialState, {
      type: 'healthReceived',
      health: healthResponse([healthDevice(0, { root: rootStatus({ role: 'leaf' }) })]),
      receivedAt: 1_000,
      epoch: 0,
    });
    state = reducer(state, { type: 'epochReset' });
    expect(state.healthEpoch).toBe(0);
    state = reducer(state, { type: 'requestFailed', source: 'health', message: 'bridge unavailable' });
    state = receive([{
      cursor: 1,
      device: 0,
      ...rootStatus({ role: 'root' }),
    }], 2_000, 1)(state);
    expect(state.rootRecords[0].role).toBe('root');
    state = reducer(state, {
      type: 'healthReceived',
      health: { ...healthResponse([healthDevice(0, { root: rootStatus({ role: 'leaf' }) })]), oldest_cursor: 1, current_cursor: 1 },
      receivedAt: 3_000,
      epoch: 1,
    });
    expect(state.healthEpoch).toBe(1);
    expect(state.health?.devices[0].root?.role).toBe('leaf');
  });

  it('shows loading, offline, stale, and reconnecting bridge states without discarding data', () => {
    expect(bridgeStatus(initialState, 0).kind).toBe('loading');
    const offline = reducer(initialState, { type: 'requestFailed', source: 'events', message: 'refused' });
    expect(bridgeStatus(offline, 0).kind).toBe('offline');
    const fresh = receive([eventRecord()], 1_000)();
    expect(bridgeStatus(fresh, 2_000).kind).toBe('online');
    expect(bridgeStatus(fresh, 16_000).kind).toBe('stale');
    const reconnecting = reducer(fresh, { type: 'requestFailed', source: 'events', message: 'reset' });
    expect(bridgeStatus(reconnecting, 2_000).kind).toBe('reconnecting');
  });
});
