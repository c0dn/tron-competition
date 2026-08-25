import type { EventRecord, EventsResponse, GttEntry, GttSnapshot, HealthDevice, HealthResponse, HealthRoot, LayoutReadyResponse } from '../lib/api';

export const SESSION_ID = '0123456789abcdef0123456789abcdef';

export function eventRecord(overrides: Partial<EventRecord> = {}): EventRecord {
  return {
    cursor: 1,
    received_at_ms: 10_000,
    kind: 'event',
    device: 0,
    now: 1234,
    root: '1842de524add',
    wearable: 7,
    packet: '00002a',
    schema: 1,
    event: 3,
    confidence: 75,
    svm: 2400,
    mic: 86,
    seq: 42,
    observer: 'dc4b0a0603f8',
    observer_rssi_dbm: null,
    path: 'tavrn',
    ...overrides,
  };
}

export function eventsResponse(
  events: EventsResponse['events'] = [],
  gap = false,
  sessionId = SESSION_ID,
): EventsResponse {
  const current = events.length === 0 ? 0 : events[events.length - 1].cursor;
  return {
    schema: 'mind.api.v2',
    session_id: sessionId,
    gap,
    oldest_cursor: events.length === 0 ? 1 : events[0].cursor,
    current_cursor: current,
    events,
  };
}

export function rootStatus(overrides: Partial<HealthRoot> = {}): HealthRoot {
  return {
    cursor: 1,
    kind: 'root',
    now: 1234,
    local: '1842de524add',
    node: 1,
    role: 'leaf',
    roots: 0,
    announced: 0,
    acked: 0,
    rejected: 0,
    pending: 0,
    rootless_drop: 0,
    ...overrides,
  };
}

export function gttEntry(overrides: Partial<GttEntry> = {}): GttEntry {
  return {
    index: 0,
    adva: '0102545678c0',
    last: 90,
    soft: 110,
    hard: 120,
    departed_deadline: 130,
    serial: 4,
    serial_state: 'known',
    hop: 2,
    hop_state: 'known',
    freshness: 'active',
    departed: 'false',
    ...overrides,
  };
}

export function gttSnapshot(overrides: Partial<GttSnapshot> = {}): GttSnapshot {
  const entries = overrides.entries ?? [gttEntry()];
  return {
    generation: 1,
    completed_at_ms: 10_000,
    query_at_ms: 101,
    local: '1842de524add',
    entry_count: entries.length,
    nondeparted_count: entries.filter((entry) => entry.departed !== 'true').length,
    entries,
    ...overrides,
  };
}

export function healthDevice(device: number, overrides: Partial<HealthDevice> = {}): HealthDevice {
  return {
    device,
    path: `/dev/serial/by-id/mind-${device}`,
    connected: true,
    parse_errors: 0,
    overlong_lines: 0,
    reconnects: 0,
    last_record_cursor: 0,
    root: null,
    gtt: null,
    owner_device: device,
    ...overrides,
  };
}

export function healthResponse(devices: HealthDevice[] = [healthDevice(0)], sessionId = SESSION_ID): HealthResponse {
  const current = Math.max(0, ...devices.map((device) => device.root?.cursor ?? 0));
  return {
    schema: 'mind.health.v2',
    session_id: sessionId,
    oldest_cursor: 1,
    current_cursor: current,
    devices,
  };
}

export function layoutReady(overrides: Partial<LayoutReadyResponse> = {}): LayoutReadyResponse {
  return {
    schema: 'mind.dashboard.layout.v1',
    status: 'ready',
    error: null,
    revision: 0,
    floorplan: null,
    positions: [],
    ...overrides,
  };
}

export function jsonResponse(status: number, body: unknown): Response {
  return {
    status,
    json: async () => body,
  } as Response;
}
