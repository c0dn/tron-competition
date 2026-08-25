import { describe, expect, it } from 'vitest';
import { eventRecord, eventsResponse, healthDevice, healthResponse, rootStatus, SESSION_ID } from '../test/fixtures';
import {
  EVIDENCE_LIMIT,
  FEED_LIMIT,
  PROCESSED_CURSOR_LIMIT,
  TOMBSTONE_LIMIT,
  UINT32_MAX,
  bridgeStatus,
  initialState,
  reducer,
  saturatingIncrement,
} from './store';

function receive(events = eventsResponse().events, at = 10_000, epoch = 0, gap = false) {
  return (state = initialState) => reducer(state, {
    type: 'eventsReceived',
    page: eventsResponse(events, gap),
    receivedAt: at,
    epoch,
  });
}

function receivePages(
  events: ReturnType<typeof eventsResponse>['events'],
  state = initialState,
  firstReceivedAt = 10_000,
  epoch = 0,
) {
  let next = state;
  for (let offset = 0; offset < events.length; offset += 100) {
    const page = events.slice(offset, offset + 100);
    next = receive(page, firstReceivedAt + (offset / 100), epoch)(next);
  }
  return next;
}

describe('dashboard event store', () => {
  it('globally deduplicates by wearable and packet across roots and observers', () => {
    const first = eventRecord({ cursor: 10, root: '1842de524add', observer: 'dc4b0a0603f8', observer_rssi_dbm: null });
    const secondRoot = eventRecord({ cursor: 11, device: 1, root: '1842de524aee', observer: 'dc4b0a0603f9', observer_rssi_dbm: -68, path: 'local' });
    let state = receive([first])();
    state = receive([secondRoot])(state);

    expect(state.feedKeys).toHaveLength(1);
    expect(state.logicalEvents['7:00002a'].evidence).toHaveLength(2);
    expect(state.logicalEvents['7:00002a'].evidence[1]?.observerRssiDbm).toBe(-68);
    expect(state.wearables[7].record.cursor).toBe(11);
  });

  it('makes a replayed processed cursor a total no-op', () => {
    const report = eventRecord({ cursor: 10 });
    const state = receive([report], 10_000)();

    expect(receive([report], 11_000)(state)).toBe(state);
  });

  it('anchors collection to first host receipt and never moves it for later clock changes', () => {
    const first = eventRecord({ cursor: 1, received_at_ms: 500 });
    const later = eventRecord({ cursor: 2, received_at_ms: 9_000, observer: 'dc4b0a0603f9' });
    const rollback = eventRecord({ cursor: 3, received_at_ms: 400, observer: 'dc4b0a0603fa' });
    let state = receive([first], 10_000)();
    expect(state.logicalEvents['7:00002a']).toMatchObject({ firstReceivedAt: 500, collectUntil: 2_500 });
    expect(state.tombstones['7:00002a']).toMatchObject({ firstReceivedAt: 500, collectUntil: 2_500 });

    state = receive([later], 20_000)(state);
    expect(state.logicalEvents['7:00002a']).toMatchObject({ firstReceivedAt: 500, collectUntil: 2_500 });
    expect(state.tombstones['7:00002a']).toMatchObject({ firstReceivedAt: 500, collectUntil: 2_500 });

    state = receive([rollback], 30_000)(state);
    expect(state.logicalEvents['7:00002a']).toMatchObject({ firstReceivedAt: 500, collectUntil: 2_500 });
    expect(state.tombstones['7:00002a']).toMatchObject({ firstReceivedAt: 500, collectUntil: 2_500 });
  });

  it('aggregates samples by observer and deterministically selects canonical representatives', () => {
    const observer = 'dc4b0a0603f8';
    let state = receive([
      eventRecord({ cursor: 1, observer, root: 'ffffffffffff', path: 'tavrn', device: 9, observer_rssi_dbm: null }),
      eventRecord({ cursor: 2, observer, root: '000000000000', path: 'local', device: 1, observer_rssi_dbm: null }),
    ])();
    expect(state.logicalEvents['7:00002a'].evidence).toEqual([{
      observer,
      root: '000000000000',
      path: 'local',
      device: 1,
      observerRssiDbm: null,
      sampleCount: 2,
    }]);

    state = receive([
      eventRecord({ cursor: 3, observer, root: 'ffffffffffff', path: 'tavrn', device: 9, observer_rssi_dbm: -80 }),
      eventRecord({ cursor: 4, observer, root: '000000000000', path: 'local', device: 1, observer_rssi_dbm: -90 }),
    ])(state);
    expect(state.logicalEvents['7:00002a'].evidence).toEqual([{
      observer,
      root: 'ffffffffffff',
      path: 'tavrn',
      device: 9,
      observerRssiDbm: -80,
      sampleCount: 4,
    }]);

    state = receive([
      eventRecord({ cursor: 5, observer, root: 'ffffffffffff', path: 'tavrn', device: 9, observer_rssi_dbm: -70 }),
      eventRecord({ cursor: 6, observer, root: '000000000001', path: 'tavrn', device: 9, observer_rssi_dbm: -70 }),
      eventRecord({ cursor: 7, observer, root: '000000000001', path: 'local', device: 9, observer_rssi_dbm: -70 }),
      eventRecord({ cursor: 8, observer, root: '000000000001', path: 'local', device: 1, observer_rssi_dbm: -70 }),
    ])(state);

    expect(state.logicalEvents['7:00002a'].evidence).toEqual([{
      observer,
      root: '000000000001',
      path: 'local',
      device: 1,
      observerRssiDbm: -70,
      sampleCount: 8,
    }]);
  });

  it('rejects new observers beyond capacity while retained observers continue aggregating', () => {
    const reports = Array.from({ length: EVIDENCE_LIMIT }, (_, index) => eventRecord({
      cursor: index + 1,
      device: index,
      root: `${index.toString(16).padStart(12, '0')}`,
      observer: `${(index + 20).toString(16).padStart(12, '0')}`,
      observer_rssi_dbm: -100 + index,
    }));
    let state = receive(reports)();
    state = receive([
      eventRecord({ ...reports[0], cursor: 17, root: 'ffffffffffff', observer_rssi_dbm: -20 }),
      eventRecord({ cursor: 18, observer: 'dc4b0a0603ff' }),
      eventRecord({ cursor: 19, observer: 'dc4b0a0603fe' }),
      eventRecord({ ...reports[0], cursor: 20, root: '000000000000', observer_rssi_dbm: -10 }),
    ])(state);

    const logical = state.logicalEvents['7:00002a'];
    expect(logical.evidence).toHaveLength(EVIDENCE_LIMIT);
    expect(logical.evidence.find((evidence) => evidence.observer === reports[0].observer)).toMatchObject({
      root: '000000000000', observerRssiDbm: -10, sampleCount: 3,
    });
    expect(logical.evidence.some((evidence) => evidence.observer === 'dc4b0a0603ff')).toBe(false);
    expect(logical.evidenceOverflowCount).toBe(2);
    expect(logical.evidenceSaturated).toBe(true);
  });

  it('bounds active, tombstone, and cursor state with FIFO tombstone eviction at 513 identities', () => {
    const records = Array.from({ length: TOMBSTONE_LIMIT + 1 }, (_, index) => {
      const value = index + 1;
      return eventRecord({
        cursor: value,
        wearable: (index % 254) + 1,
        packet: value.toString(16).padStart(6, '0'),
        seq: value & 0xff,
      });
    });
    expect(FEED_LIMIT).toBe(256);
    expect(TOMBSTONE_LIMIT).toBe(512);
    expect(records).toHaveLength(513);
    let state = receivePages(records);
    expect(state.feedKeys).toHaveLength(FEED_LIMIT);
    expect(Object.keys(state.logicalEvents)).toHaveLength(FEED_LIMIT);
    expect(state.processedCursors).toHaveLength(PROCESSED_CURSOR_LIMIT);
    expect(Object.keys(state.tombstones)).toHaveLength(TOMBSTONE_LIMIT);
    expect(state.tombstoneKeys).toHaveLength(TOMBSTONE_LIMIT);
    expect(state.tombstones['1:000001']).toBeUndefined();
    expect(state.tombstoneKeys[0]).toBe('2:000002');

    state = receive([eventRecord({ cursor: TOMBSTONE_LIMIT + 2, wearable: 1, packet: '000001', seq: 1, confidence: 76 })], 20_000)(state);
    expect(state.logicalEvents['1:000001']).toMatchObject({
      conflict: false,
      firstReceivedAt: 10_000,
      collectUntil: 12_000,
    });
  });

  it('keeps active-feed-evicted late reports tombstoned without corrupting active feed order', () => {
    const first = eventRecord({ cursor: 1, wearable: 7, packet: '000001', seq: 1 });
    const fillers = Array.from({ length: FEED_LIMIT }, (_, index) => {
      const value = index + 100;
      return eventRecord({
        cursor: index + 2,
        wearable: (index % 254) + 1,
        packet: value.toString(16).padStart(6, '0'),
        seq: value & 0xff,
      });
    });
    expect(fillers).toHaveLength(256);
    let state = receivePages([first, ...fillers], initialState, 100);
    const key = '7:000001';
    const activeFeedKeys = state.feedKeys;
    const activeLogicalEvents = state.logicalEvents;
    const tombstoneKeys = state.tombstoneKeys;
    const canonicalTombstone = state.tombstones[key];
    expect(state.logicalEvents[key]).toBeUndefined();
    expect(canonicalTombstone).toMatchObject({
      fingerprint: '1:3:75:2400:86:1',
      conflict: false,
      firstReceivedAt: 10_000,
      collectUntil: 12_000,
    });

    state = receive([eventRecord({ cursor: FEED_LIMIT + 2, wearable: 7, packet: '000001', seq: 1 })], 5_000)(state);
    expect(state.logicalEvents).toBe(activeLogicalEvents);
    expect(state.feedKeys).toBe(activeFeedKeys);
    expect(state.tombstoneKeys).toBe(tombstoneKeys);
    expect(state.tombstones[key]).toEqual(canonicalTombstone);
    expect(state.wearables[7].record.cursor).toBe(FEED_LIMIT + 2);

    state = receive([eventRecord({
      cursor: FEED_LIMIT + 3, wearable: 7, packet: '000001', seq: 1, confidence: 76,
    })], 5_001)(state);
    expect(state.logicalEvents).toBe(activeLogicalEvents);
    expect(state.feedKeys).toBe(activeFeedKeys);
    expect(state.tombstoneKeys).toBe(tombstoneKeys);
    expect(state.tombstones[key]).toEqual({ ...canonicalTombstone, conflict: true });
    expect(state.wearables[7].record.cursor).toBe(FEED_LIMIT + 3);
    expect(state.conflictCount).toBe(1);
  });

  it('keeps an active event authoritative after its non-refreshed tombstone FIFO entry is evicted', () => {
    const first = eventRecord({ cursor: 1, wearable: 7, packet: '000001', seq: 1 });
    const initialFillers = Array.from({ length: FEED_LIMIT - 1 }, (_, index) => {
      const value = index + 100;
      return eventRecord({
        cursor: index + 2,
        wearable: (index % 254) + 1,
        packet: value.toString(16).padStart(6, '0'),
        seq: value & 0xff,
      });
    });
    const laterFillers = Array.from({ length: TOMBSTONE_LIMIT - FEED_LIMIT - 1 }, (_, index) => {
      const value = index + 1_000;
      return eventRecord({
        cursor: FEED_LIMIT + index + 2,
        wearable: (index % 254) + 1,
        packet: value.toString(16).padStart(6, '0'),
        seq: value & 0xff,
      });
    });
    expect(initialFillers).toHaveLength(255);
    expect(laterFillers).toHaveLength(255);
    let state = receivePages([first, ...initialFillers], initialState, 100);
    state = receive([eventRecord({ cursor: FEED_LIMIT + 1, wearable: 7, packet: '000001', seq: 1 })], 200)(state);
    state = receivePages(laterFillers, state, 300);
    state = receive([eventRecord({ cursor: TOMBSTONE_LIMIT + 1, wearable: 7, packet: '000001', seq: 1 })], 400)(state);
    state = receive([eventRecord({ cursor: TOMBSTONE_LIMIT + 2, wearable: 8, packet: 'fff001', seq: 255 })], 500)(state);
    state = receive([eventRecord({ cursor: TOMBSTONE_LIMIT + 3, wearable: 8, packet: 'fff002', seq: 2 })], 600)(state);
    expect(state.tombstones['7:000001']).toBeUndefined();
    expect(state.logicalEvents['7:000001']).toMatchObject({ firstReceivedAt: 10_000, collectUntil: 12_000 });

    state = receive([eventRecord({ cursor: TOMBSTONE_LIMIT + 4, wearable: 7, packet: '000001', seq: 1, confidence: 76 })], 5_000)(state);
    expect(state.tombstones['7:000001']).toBeUndefined();
    expect(state.logicalEvents['7:000001']).toMatchObject({
      conflict: true,
      firstReceivedAt: 10_000,
      collectUntil: 12_000,
      evidence: [expect.objectContaining({ sampleCount: 4 })],
    });
  });

  it('saturates sample, evidence-overflow, conflict, and gap counters at uint32 maximum', () => {
    let state = receive([eventRecord({ cursor: 1 })])();
    const key = '7:00002a';
    const logical = state.logicalEvents[key];
    state = {
      ...state,
      logicalEvents: {
        ...state.logicalEvents,
        [key]: { ...logical, evidence: [{ ...logical.evidence[0], sampleCount: UINT32_MAX - 1 }] },
      },
    };
    state = receive([eventRecord({ cursor: 2 })])(state);
    state = receive([eventRecord({ cursor: 3 })])(state);
    expect(state.logicalEvents[key].evidence[0]?.sampleCount).toBe(UINT32_MAX);

    const fullEvidence = Array.from({ length: EVIDENCE_LIMIT }, (_, index) => ({
      ...eventRecord({ cursor: index + 10, observer: `${(index + 20).toString(16).padStart(12, '0')}` }),
    }));
    state = receive(fullEvidence)();
    state = {
      ...state,
      logicalEvents: {
        ...state.logicalEvents,
        [key]: { ...state.logicalEvents[key], evidenceOverflowCount: UINT32_MAX - 1, evidenceSaturated: true },
      },
    };
    state = receive([eventRecord({ cursor: 30, observer: 'dc4b0a0603ff' })])(state);
    state = receive([eventRecord({ cursor: 31, observer: 'dc4b0a0603fe' })])(state);
    expect(state.logicalEvents[key].evidenceOverflowCount).toBe(UINT32_MAX);

    state = receive([eventRecord({ cursor: 1 })])();
    state = { ...state, conflictCount: UINT32_MAX - 1 };
    state = receive([eventRecord({ cursor: 2, confidence: 76 })])(state);
    state = receive([eventRecord({ cursor: 3, confidence: 77 })])(state);
    expect(state.conflictCount).toBe(UINT32_MAX);

    state = { ...initialState, gapCount: UINT32_MAX - 1 };
    state = receive([eventRecord({ cursor: 1 })], 10_000, 0, true)(state);
    state = receive([eventRecord({ cursor: 2, packet: '00002b', seq: 43 })], 10_001, 0, true)(state);
    expect(state.gapCount).toBe(UINT32_MAX);
    expect(saturatingIncrement(UINT32_MAX)).toBe(UINT32_MAX);
  });

  it('marks a cursor gap while continuing from the records supplied by the bridge', () => {
    const page = eventsResponse([eventRecord({ cursor: 40 })], true);
    const state = reducer(initialState, { type: 'eventsReceived', page, receivedAt: 10_000, epoch: 0 });
    expect(state.lastGap).toEqual({ oldest_cursor: 40, current_cursor: 40 });
    expect(state.eventCursor).toBe(40);
    expect(state.feedKeys).toEqual(['7:00002a']);
  });

  it('clears all process-session records before accepting lower cursors from the next bridge session', () => {
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
    expect(state.logicalEvents).toEqual({});
    expect(state.tombstones).toEqual({});
    expect(state.health).toBeNull();

    state = receive([eventRecord({ cursor: 1, wearable: 8, packet: '000001', seq: 1 })], 11_000, 1)(state);
    expect(state.wearables[8]).toMatchObject({ epoch: 1, record: { cursor: 1 } });
    state = receive([eventRecord({ cursor: 2, packet: '00002a', confidence: 76 })], 12_000, 1)(state);
    expect(state.wearables[7]).toMatchObject({ epoch: 1, record: { cursor: 2 } });
    expect(state.logicalEvents['7:00002a']).toMatchObject({
      conflict: false,
      firstReceivedAt: 10_000,
      collectUntil: 12_000,
      evidence: [expect.objectContaining({ sampleCount: 1 })],
    });
  });

  it('rejects stale health from a prior session instead of restoring its authority', () => {
    let state = reducer(initialState, {
      type: 'healthReceived',
      health: healthResponse([healthDevice(0, { root: rootStatus({ role: 'leaf' }) })]),
      receivedAt: 1_000,
      epoch: 0,
    });
    state = reducer(state, { type: 'epochReset' });
    expect(state.healthEpoch).toBeNull();
    state = reducer(state, { type: 'requestFailed', source: 'health', message: 'bridge unavailable' });
    state = receive([{
      device: 0,
      ...rootStatus({ role: 'root' }),
      cursor: 1,
    }], 2_000, 1)(state);
    expect(state.rootRecords[0].role).toBe('root');
    state = reducer(state, {
      type: 'healthReceived',
      health: {
        ...healthResponse([healthDevice(0, { root: rootStatus({ role: 'leaf' }) })], 'fedcba9876543210fedcba9876543210'),
        oldest_cursor: 1,
        current_cursor: 1,
      },
      receivedAt: 3_000,
      epoch: 1,
    });
    expect(state.healthEpoch).toBeNull();
    expect(state.health).toBeNull();
    expect(state.rootRecords[0].role).toBe('root');
  });

  it('makes a changed session authoritative even when its cursor is higher and discards old authority', () => {
    const nextSession = 'fedcba9876543210fedcba9876543210';
    let state = reducer(initialState, { type: 'sessionReset', sessionId: SESSION_ID });
    state = receive([{
      cursor: 10,
      kind: 'root',
      device: 0,
      now: 1,
      local: '1842de524add',
      node: 1,
      role: 'root',
      roots: 1,
      announced: 1,
      acked: 1,
      rejected: 0,
      pending: 0,
      rootless_drop: 0,
    }, eventRecord({ cursor: 11, wearable: 7 })], 10_000, 1)(state);

    state = reducer(state, { type: 'sessionReset', sessionId: nextSession });
    expect(state).toMatchObject({
      sessionId: nextSession,
      eventCursor: 0,
      processedCursors: [],
      health: null,
      rootRecords: {},
      commandRecords: {},
      pendingRoot: {},
      logicalEvents: {},
    });

    state = reducer(state, {
      type: 'eventsReceived',
      page: eventsResponse([eventRecord({ cursor: 50, wearable: 8, packet: '000008', seq: 8 })], false, nextSession),
      receivedAt: 20_000,
      epoch: 2,
    });
    expect(state).toMatchObject({ sessionId: nextSession, eventCursor: 50, processedCursors: [50] });
    expect(state.logicalEvents).toHaveProperty('8:000008');
    expect(state.logicalEvents).not.toHaveProperty('7:00002a');

    state = reducer(state, {
      type: 'healthReceived',
      health: healthResponse([healthDevice(0, { root: rootStatus({ role: 'root', cursor: 50 }) })], SESSION_ID),
      receivedAt: 21_000,
      epoch: 2,
    });
    expect(state.health).toBeNull();
    expect(state.rootRecords).toEqual({});
  });

  it('keeps a 202 Gateway command pending until a newer authoritative status record or health cursor confirms the desired state', () => {
    let state = reducer(initialState, {
      type: 'rootPending', device: 0, active: true, requestId: 1, baselineCursor: 5, baselineEpoch: 0,
    });
    state = reducer(state, { type: 'rootAccepted', device: 0, command: 'on', requestId: 1 });
    expect(state.pendingRoot[0]).toMatchObject({ desired: true, phase: 'confirming' });
    state = receive([{ device: 0, ...rootStatus({ role: 'root' }), cursor: 5 }])(state);
    expect(state.pendingRoot[0]).toBeDefined();
    state = receive([{ device: 0, ...rootStatus({ role: 'root' }), cursor: 6 }])(state);
    expect(state.pendingRoot[0]).toBeUndefined();
    expect(state.announcement).toBe('Gateway confirmed ON from an authoritative Gateway status record.');

    state = reducer(state, {
      type: 'rootPending', device: 0, active: false, requestId: 2, baselineCursor: 6, baselineEpoch: 0,
    });
    state = reducer(state, {
      type: 'healthReceived',
      health: { ...healthResponse([healthDevice(0, { root: rootStatus({ role: 'leaf', cursor: 7 }) })]), oldest_cursor: 1, current_cursor: 7 },
      receivedAt: 7_000,
      epoch: 0,
    });
    expect(state.pendingRoot[0]).toBeUndefined();
    expect(state.announcement).toBe('Gateway confirmed OFF from the authoritative Gateway health snapshot.');
  });

  it('clears a post-baseline authoritative role mismatch immediately and replaces waiting copy with the mismatch', () => {
    let state = reducer(initialState, {
      type: 'rootPending', device: 0, active: true, requestId: 1, baselineCursor: 5, baselineEpoch: 0,
    });
    state = reducer(state, { type: 'rootAccepted', device: 0, command: 'on', requestId: 1 });
    state = receive([{ device: 0, ...rootStatus({ role: 'leaf' }), cursor: 6 }])(state);
    expect(state.pendingRoot[0]).toBeUndefined();
    expect(state.announcement).toBe('Gateway reported OFF from an authoritative Gateway status record; requested ON did not match.');

    state = reducer(state, {
      type: 'rootPending', device: 0, active: false, requestId: 2, baselineCursor: 6, baselineEpoch: 0,
    });
    state = reducer(state, {
      type: 'healthReceived',
      health: { ...healthResponse([healthDevice(0, { root: rootStatus({ role: 'root', cursor: 7 }) })]), oldest_cursor: 1, current_cursor: 7 },
      receivedAt: 7_000,
      epoch: 0,
    });
    expect(state.pendingRoot[0]).toBeUndefined();
    expect(state.announcement).toBe('Gateway reported ON from the authoritative Gateway health snapshot; requested OFF did not match.');
  });

  it('treats matching post-baseline busy and rejection statuses as terminal firmware failures and ignores stale actions', () => {
    let state = reducer(initialState, {
      type: 'rootPending', device: 0, active: true, requestId: 2, baselineCursor: 5, baselineEpoch: 0,
    });
    state = receive([{
      cursor: 6, kind: 'command', device: 0, now: 1, local: '1842de524add', command: 'on', status: 'busy',
    }])(state);
    expect(state.pendingRoot[0]).toBeUndefined();
    expect(state.announcement).toBe('Gateway firmware command ON failed: busy.');
    state = reducer(state, {
      type: 'rootPending', device: 0, active: true, requestId: 3, baselineCursor: 6, baselineEpoch: 0,
    });
    state = receive([{
      cursor: 7, kind: 'command', device: 0, now: 2, local: '1842de524add', command: 'on', status: 'rejected',
    }])(state);
    expect(state.pendingRoot[0]).toBeUndefined();

    state = reducer(state, {
      type: 'rootPending', device: 0, active: false, requestId: 4, baselineCursor: 7, baselineEpoch: 0,
    });
    state = reducer(state, { type: 'rootFailed', device: 0, requestId: 3, message: 'stale failure' });
    state = reducer(state, { type: 'rootConfirmationTimedOut', device: 0, requestId: 3 });
    expect(state.pendingRoot[0]).toMatchObject({ requestId: 4, desired: false });
  });

  it('records GTT command evidence without overwriting Gateway-control status copy', () => {
    const state = receive([{
      cursor: 1, kind: 'command', device: 0, now: 1, local: '1842de524add', command: 'gtt', status: 'accepted',
    }])({ ...initialState, announcement: 'Gateway controls are ready.' });

    expect(state.commandRecords[0]).toMatchObject({ command: 'gtt', status: 'accepted' });
    expect(state.announcement).toBe('Gateway controls are ready.');
  });

  it('resets epoch-bound root confirmation state and does not let an old timeout clear a new request', () => {
    let state = reducer(initialState, {
      type: 'rootPending', device: 0, active: true, requestId: 1, baselineCursor: 4, baselineEpoch: 0,
    });
    state = reducer(state, { type: 'epochReset' });
    expect(state.pendingRoot).toEqual({});
    state = reducer(state, {
      type: 'rootPending', device: 0, active: false, requestId: 2, baselineCursor: 0, baselineEpoch: 1,
    });
    state = reducer(state, { type: 'rootConfirmationTimedOut', device: 0, requestId: 1 });
    expect(state.pendingRoot[0]).toMatchObject({ requestId: 2, desired: false });
    state = reducer(state, { type: 'rootConfirmationTimedOut', device: 0, requestId: 2 });
    expect(state.pendingRoot[0]).toBeUndefined();
  });

  it('does not resolve root pending from unrelated aggregate cursor movement, but accepts a newer root-specific health cursor', () => {
    let state = reducer(initialState, {
      type: 'rootPending', device: 0, active: true, requestId: 1, baselineCursor: 5, baselineEpoch: 0,
    });
    state = reducer(state, {
      type: 'healthReceived',
      health: {
        ...healthResponse([healthDevice(0, { root: rootStatus({ role: 'root', cursor: 5 }) })]),
        oldest_cursor: 1,
        current_cursor: 100,
      },
      receivedAt: 1_000,
      epoch: 0,
    });
    expect(state.pendingRoot[0]).toBeDefined();

    state = reducer(state, {
      type: 'healthReceived',
      health: {
        ...healthResponse([healthDevice(0, { root: rootStatus({ role: 'root', cursor: 6 }) })]),
        oldest_cursor: 1,
        current_cursor: 100,
      },
      receivedAt: 2_000,
      epoch: 0,
    });
    expect(state.pendingRoot[0]).toBeUndefined();
    expect(state.announcement).toBe('Gateway confirmed ON from the authoritative Gateway health snapshot.');
  });

  it('clears stale Gateway command errors only when a newer authoritative status supersedes them', () => {
    let state = reducer(initialState, {
      type: 'rootPending', device: 0, active: true, requestId: 1, baselineCursor: 5, baselineEpoch: 0,
    });
    state = reducer(state, { type: 'rootFailed', device: 0, requestId: 1, message: 'Gateway command could not be completed. Retry Gateway.' });
    expect(state.rootErrors[0]).toBeDefined();
    state = receive([{ device: 0, ...rootStatus({ role: 'leaf' }), cursor: 5 }])(state);
    expect(state.rootErrors[0]).toBeDefined();
    state = receive([{ device: 0, ...rootStatus({ role: 'leaf' }), cursor: 6 }])(state);
    expect(state.rootErrors[0]).toBeUndefined();

    state = reducer(state, {
      type: 'rootPending', device: 0, active: false, requestId: 2, baselineCursor: 6, baselineEpoch: 0,
    });
    state = reducer(state, { type: 'rootConfirmationTimedOut', device: 0, requestId: 2 });
    state = reducer(state, {
      type: 'healthReceived',
      health: { ...healthResponse([healthDevice(0, { root: rootStatus({ role: 'root', cursor: 6 }) })]), current_cursor: 6 },
      receivedAt: 7_000,
      epoch: 0,
    });
    expect(state.rootErrors[0]).toBeDefined();
    state = reducer(state, {
      type: 'healthReceived',
      health: { ...healthResponse([healthDevice(0, { root: rootStatus({ role: 'root', cursor: 7 }) })]), current_cursor: 7 },
      receivedAt: 8_000,
      epoch: 0,
    });
    expect(state.rootErrors[0]).toBeUndefined();
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
