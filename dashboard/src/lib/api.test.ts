import { execFileSync } from 'node:child_process';
import { resolve } from 'node:path';
import { afterEach, describe, expect, it, vi } from 'vitest';
import {
  ApiError,
  DecodeError,
  decodeEventsResponse,
  decodeHealthResponse,
  fetchEvents,
  postRoot,
} from './api';
import { eventRecord, eventsResponse, healthResponse, jsonResponse } from '../test/fixtures';
import { restoreFetch, stubFetch } from '../test/runtime';

afterEach(restoreFetch);

describe('frozen bridge API decoders', () => {
  it('decodes empty and populated JSON emitted by the real host.bridge implementation', () => {
    const script = [
      'import json',
      'from host import bridge',
      'state = bridge.Bridge(["test"])',
      'empty_events = state.ring.page(0, 100)',
      'empty_health = state.health()',
      'state.devices[0].feed_bytes(b"mind_event_v1 now=55 root=8081545678c0 wearable=7 packet=a1b2c3 schema=1 event=5 confidence=73 svm=6687 mic=125 seq=195 observer=0102545678c0 path=tavrn\\n")',
      'state.devices[0].feed_bytes(b"mind_root_v1 now=99 local=8081545678c0 node=6 role=root roots=16 announced=2 acked=1 rejected=3 pending=4 rootless_drop=7\\n")',
      'print(json.dumps({"empty_events": empty_events, "empty_health": empty_health, "events": state.ring.page(0, 100), "health": state.health()}))',
    ].join('; ');
    const output = execFileSync('python3', ['-c', script], {
      cwd: resolve(process.cwd(), '..'),
      encoding: 'utf8',
    });
    const contract = JSON.parse(output) as Record<string, unknown>;

    expect(decodeEventsResponse(contract.empty_events, 0)).toMatchObject({ oldest_cursor: 1, current_cursor: 0, events: [] });
    expect(decodeHealthResponse(contract.empty_health)).toMatchObject({ oldest_cursor: 1, current_cursor: 0 });
    expect(decodeEventsResponse(contract.events, 0).events).toHaveLength(2);
    expect(decodeHealthResponse(contract.health).devices[0].root).toMatchObject({ kind: 'root', node: 6, role: 'root' });
  });

  it('accepts exact event and health records but rejects malformed or surplus fields', () => {
    const page = eventsResponse([eventRecord()]);
    expect(decodeEventsResponse(page).events).toHaveLength(1);
    expect(decodeHealthResponse(healthResponse()).devices[0].device).toBe(0);

    expect(() => decodeEventsResponse({ ...page, unexpected: true })).toThrow(DecodeError);
    expect(() => decodeEventsResponse({ ...page, events: [{ ...page.events[0], packet: 'BAD' }] })).toThrow(DecodeError);
    expect(() => decodeEventsResponse({ ...page, events: [{ ...page.events[0], seq: 41 }] })).toThrow(DecodeError);
    expect(() => decodeHealthResponse({ ...healthResponse(), devices: [{ ...healthResponse().devices[0], connected: 'yes' }] })).toThrow(DecodeError);
  });

  it('enforces firmware bounds, the one allowed empty ring, and page/cursor relationships', () => {
    const page = eventsResponse([eventRecord({ cursor: 1, svm: 8000 })]);
    expect(decodeEventsResponse(page, 0).events[0]).toMatchObject({ kind: 'event', svm: 8000 });
    expect(decodeEventsResponse(eventsResponse([eventRecord({ event: 0, confidence: 0, mic: 0 })]), 0).events[0]).toMatchObject({ kind: 'event', event: 0 });
    expect(() => decodeEventsResponse({ ...page, oldest_cursor: 0 })).toThrow(DecodeError);
    expect(() => decodeHealthResponse({ ...healthResponse(), oldest_cursor: 0 })).toThrow(DecodeError);
    expect(() => decodeEventsResponse(eventsResponse([eventRecord({ svm: 8001 })]), 0)).toThrow(DecodeError);
    expect(() => decodeEventsResponse(eventsResponse([eventRecord({ event: 0, confidence: 1, mic: 0 })]), 0)).toThrow(DecodeError);
    expect(() => decodeEventsResponse(eventsResponse([eventRecord({ event: 0, confidence: 0, mic: 1 })]), 0)).toThrow(DecodeError);
    expect(() => decodeHealthResponse(healthResponse([{
      ...healthResponse().devices[0],
      root: { kind: 'root', now: 1, local: '1842de524add', node: 7, role: 'root', roots: 1, announced: 0, acked: 0, rejected: 0, pending: 0, rootless_drop: 0 },
    }]))).toThrow(DecodeError);

    const oversized = Array.from({ length: 101 }, (_, index) => eventRecord({ cursor: index + 1 }));
    expect(() => decodeEventsResponse({ ...eventsResponse(oversized), current_cursor: 101 }, 0)).toThrow(DecodeError);
    expect(() => decodeEventsResponse({ ...page, current_cursor: 2 }, 0)).toThrow(DecodeError);
    expect(() => decodeEventsResponse(eventsResponse([eventRecord({ cursor: 2 })]), 2)).toThrow(DecodeError);
    expect(() => decodeEventsResponse({ ...eventsResponse([eventRecord({ cursor: 2 })], false), oldest_cursor: 2, current_cursor: 2 }, 0)).toThrow(DecodeError);
    expect(decodeEventsResponse({ schema: 'mind.api.v1', gap: false, oldest_cursor: 1, current_cursor: 4, events: [] }, 9)).toMatchObject({ current_cursor: 4 });
    expect(() => decodeEventsResponse({ schema: 'mind.api.v1', gap: true, oldest_cursor: 1, current_cursor: 4, events: [] }, 9)).toThrow(DecodeError);
  });

  it('surfaces HTTP API errors and rejects a malformed successful response before state can consume it', async () => {
    const fetchMock = vi.fn()
      .mockResolvedValueOnce(jsonResponse(503, { schema: 'mind.error.v1', accepted: false, error: 'disconnected' }))
      .mockResolvedValueOnce(jsonResponse(200, { schema: 'mind.api.v1', gap: false, oldest_cursor: 1, current_cursor: 0, events: [{}] }));
    stubFetch(fetchMock);

    await expect(fetchEvents(0)).rejects.toBeInstanceOf(ApiError);
    await expect(fetchEvents(0)).rejects.toBeInstanceOf(DecodeError);
  });

  it('validates fetched pages against the requested exclusive cursor while allowing a reset response', async () => {
    const fetchMock = vi.fn()
      .mockResolvedValueOnce(jsonResponse(200, { schema: 'mind.api.v1', gap: false, oldest_cursor: 1, current_cursor: 2, events: [eventRecord({ cursor: 2 })] }))
      .mockResolvedValueOnce(jsonResponse(200, { schema: 'mind.api.v1', gap: false, oldest_cursor: 1, current_cursor: 2, events: [] }));
    stubFetch(fetchMock);

    await expect(fetchEvents(2)).rejects.toBeInstanceOf(DecodeError);
    await expect(fetchEvents(9)).resolves.toMatchObject({ current_cursor: 2, events: [] });
  });

  it('sends the exact root command body and validates the accepted command response', async () => {
    const fetchMock = vi.fn().mockResolvedValue(jsonResponse(202, {
      schema: 'mind.command.v1', accepted: true, device: 3, command: 'on',
    }));
    stubFetch(fetchMock);

    await expect(postRoot(3, true)).resolves.toMatchObject({ device: 3, command: 'on' });
    expect(fetchMock).toHaveBeenCalledWith('/api/root', expect.objectContaining({
      method: 'POST',
      body: '{"device":3,"active":true}',
    }));
  });

  it('rejects accepted root responses that do not echo the requested device and command', async () => {
    const fetchMock = vi.fn()
      .mockResolvedValueOnce(jsonResponse(202, {
        schema: 'mind.command.v1', accepted: true, device: 4, command: 'on',
      }))
      .mockResolvedValueOnce(jsonResponse(202, {
        schema: 'mind.command.v1', accepted: true, device: 3, command: 'off',
      }));
    stubFetch(fetchMock);

    await expect(postRoot(3, true)).rejects.toBeInstanceOf(ApiError);
    await expect(postRoot(3, true)).rejects.toBeInstanceOf(ApiError);
  });
});
