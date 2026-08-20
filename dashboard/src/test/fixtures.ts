import type { EventRecord, EventsResponse, HealthDevice, HealthResponse, HealthRoot } from '../lib/api';

export function eventRecord(overrides: Partial<EventRecord> = {}): EventRecord {
  return {
    cursor: 1,
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
    path: 'tavrn',
    ...overrides,
  };
}

export function eventsResponse(events: EventsResponse['events'] = [], gap = false): EventsResponse {
  const current = events.length === 0 ? 0 : events[events.length - 1].cursor;
  return {
    schema: 'mind.api.v1',
    gap,
    oldest_cursor: events.length === 0 ? 1 : events[0].cursor,
    current_cursor: current,
    events,
  };
}

export function rootStatus(overrides: Partial<HealthRoot> = {}): HealthRoot {
  return {
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
    ...overrides,
  };
}

export function healthResponse(devices: HealthDevice[] = [healthDevice(0)]): HealthResponse {
  return {
    schema: 'mind.health.v1',
    oldest_cursor: 1,
    current_cursor: 0,
    devices,
  };
}

export function jsonResponse(status: number, body: unknown): Response {
  return {
    status,
    json: async () => body,
  } as Response;
}
