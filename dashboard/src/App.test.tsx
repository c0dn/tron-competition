import { readFile } from 'node:fs/promises';
import './test/runtime';
import { act, cleanup, fireEvent, render, waitFor, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { afterEach, describe, expect, it, vi } from 'vitest';
import App from './App';
import { eventRecord, eventsResponse, gttEntry, gttSnapshot, healthDevice, healthResponse, jsonResponse, layoutReady, rootStatus } from './test/fixtures';
import { restoreFetch, stubFetch } from './test/runtime';

afterEach(() => {
  cleanup();
  restoreFetch();
  vi.useRealTimers();
});

function gatewayHealth() {
  const local = '1842de524add';
  return healthResponse([healthDevice(0, {
    root: rootStatus({ local, role: 'leaf' }),
    gtt: gttSnapshot({ local, entries: [gttEntry({ adva: local }), gttEntry({ index: 1, adva: '0102545678c1', freshness: 'soft_stale', departed: 'unknown' })] }),
  })]);
}

function bridgeFetch(health = gatewayHealth()) {
  return vi.fn((input: string, init?: RequestInit) => {
    if (input.startsWith('/api/events')) return Promise.resolve(jsonResponse(200, eventsResponse([eventRecord({ observer_rssi_dbm: -72 })])));
    if (input === '/api/health') return Promise.resolve(jsonResponse(200, health));
    if (input === '/api/layout') {
      return Promise.resolve(jsonResponse(init?.method === 'PUT' ? 200 : 200, layoutReady()));
    }
    if (input === '/api/root') {
      const request = JSON.parse(String(init?.body)) as { device: number; active: boolean };
      return Promise.resolve(jsonResponse(202, { schema: 'mind.command.v1', accepted: true, device: request.device, command: request.active ? 'on' : 'off' }));
    }
    if (input === '/api/gtt') return Promise.resolve(jsonResponse(202, { schema: 'mind.command.v1', accepted: true, device: 0, command: 'gtt' }));
    throw new Error(`Unexpected request ${input}`);
  });
}

function deferred<T>() {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>((complete) => {
    resolve = complete;
  });
  return { promise, resolve };
}

describe('Gateway floorplan dashboard', () => {
  it('renders one Gateway, a focal floorplan, staged GTT nodes, and no visible ROOT vocabulary', async () => {
    stubFetch(bridgeFetch());
    render(<App pollIntervalMs={60_000} />);
    const ui = within(document.body);
    await ui.findByRole('heading', { name: 'Floorplan' });
    const headings = ui.getAllByRole('heading');
    expect(headings.indexOf(ui.getByRole('heading', { name: 'Event feed' })))
      .toBeLessThan(headings.indexOf(ui.getByRole('heading', { name: 'Floorplan' })));
    expect(ui.getByRole('switch', { name: 'Turn Gateway on' })).not.toBeNull();
    expect(ui.getByText('Unpositioned nodes')).not.toBeNull();
    expect(ui.getAllByText('0102545678c1')).toHaveLength(2);
    expect(document.body.textContent).not.toMatch(/root/i);
  });

  it('sends the only Gateway toggle to device zero and retains RSSI evidence', async () => {
    const fetchMock = bridgeFetch();
    stubFetch(fetchMock);
    const user = userEvent.setup();
    render(<App pollIntervalMs={60_000} />);
    const ui = within(document.body);
    const toggle = await ui.findByRole('switch', { name: 'Turn Gateway on' });
    await user.click(toggle);
    await waitFor(() => expect(fetchMock).toHaveBeenCalledWith('/api/root', expect.objectContaining({ body: '{"device":0,"active":true}' })));
    expect(ui.getByText(/-72 dBm/)).not.toBeNull();
  });

  it('keeps stale health visible but prevents Gateway, GTT, and roster-placement authority', async () => {
    vi.useFakeTimers();
    vi.setSystemTime(new Date(1_000));
    const fetchMock = bridgeFetch();
    stubFetch(fetchMock);
    render(<App pollIntervalMs={60_000} />);
    const ui = within(document.body);
    await act(async () => { await vi.advanceTimersByTimeAsync(0); });
    const toggle = ui.getByRole('switch', { name: 'Turn Gateway on' });
    expect(ui.getAllByRole('button', { name: 'Place at center' }).every((button) => !button.hasAttribute('disabled'))).toBe(true);

    await act(async () => { await vi.advanceTimersByTimeAsync(15_000); });
    expect(ui.getByRole('status', { name: 'Bridge: Bridge data is stale' })).not.toBeNull();
    expect(toggle).toHaveProperty('disabled', true);
    expect(ui.queryByRole('button', { name: 'Place at center' })).toBeNull();
    expect(ui.queryByRole('button', { name: /GTT/i })).toBeNull();
    const gttCallsAtStale = fetchMock.mock.calls.filter(([input]) => input === '/api/gtt').length;
    fireEvent.click(toggle);
    expect(fetchMock.mock.calls.filter(([input]) => input === '/api/root')).toHaveLength(0);

    await act(async () => { await vi.advanceTimersByTimeAsync(10_000); });
    expect(fetchMock.mock.calls.filter(([input]) => input === '/api/gtt')).toHaveLength(gttCallsAtStale);
  });

  it('fails closed when lower-cursor health arrives before the event stream reports a bridge restart', async () => {
    let eventRequests = 0;
    let healthRequests = 0;
    const local = '1842de524add';
    const healthAt = (cursor: number) => ({
      ...healthResponse([healthDevice(0, {
        root: rootStatus({ cursor, local, role: 'leaf' }),
        gtt: gttSnapshot({ generation: cursor, local, entries: [gttEntry({ adva: local })] }),
      })]),
      current_cursor: cursor,
    });
    const fetchMock = vi.fn((input: string, init?: RequestInit) => {
      if (input.startsWith('/api/events')) {
        eventRequests += 1;
        if (eventRequests === 1) {
          return Promise.resolve(jsonResponse(200, eventsResponse(
            Array.from({ length: 10 }, (_, index) => eventRecord({ cursor: index + 1 })),
          )));
        }
        return new Promise<Response>((_resolve, reject) => {
          init?.signal?.addEventListener('abort', () => reject(new DOMException('Aborted', 'AbortError')));
        });
      }
      if (input === '/api/health') {
        healthRequests += 1;
        return Promise.resolve(jsonResponse(200, healthAt(healthRequests === 1 ? 10 : 1)));
      }
      if (input === '/api/layout') return Promise.resolve(jsonResponse(200, layoutReady()));
      throw new Error(`Unexpected request ${input}`);
    });
    stubFetch(fetchMock);
    render(<App pollIntervalMs={1} />);
    const ui = within(document.body);
    await ui.findByRole('switch', { name: 'Turn Gateway on' });
    await waitFor(() => expect(healthRequests).toBe(2));
    await waitFor(() => expect(ui.getByRole('checkbox', { name: 'Gateway status unknown' })).toHaveProperty('disabled', true));
    expect(ui.queryByRole('button', { name: 'Place at center' })).toBeNull();
    expect(fetchMock.mock.calls.filter(([input]) => input === '/api/root')).toHaveLength(0);
    expect(fetchMock.mock.calls.filter(([input]) => input === '/api/gtt')).toHaveLength(0);
  });

  it('discards a changed-session old-cursor page and drains the replacement from zero', async () => {
    vi.useFakeTimers();
    vi.setSystemTime(new Date(0));
    const oldSession = '0123456789abcdef0123456789abcdef';
    const nextSession = 'fedcba9876543210fedcba9876543210';
    const eventTargets: string[] = [];
    let healthPages = 0;
    const recordsAt = (count: number, firstWearable: number) => Array.from(
      { length: count },
      (_, index) => eventRecord({
        cursor: index + 1,
        wearable: firstWearable + index,
        packet: (index + 1).toString(16).padStart(6, '0'),
        seq: index + 1,
        received_at_ms: 0,
      }),
    );
    const fetchMock = vi.fn((input: string) => {
      if (input.startsWith('/api/events')) {
        eventTargets.push(input);
        if (input.includes('after=0')) {
          if (eventTargets.length === 1) {
            return Promise.resolve(jsonResponse(200, eventsResponse(recordsAt(10, 7), false, oldSession)));
          }
          return Promise.resolve(jsonResponse(200, eventsResponse(recordsAt(5, 8), false, nextSession)));
        }
        return Promise.resolve(jsonResponse(200, eventsResponse(recordsAt(5, 99), false, nextSession)));
      }
      if (input === '/api/health') {
        healthPages += 1;
        return Promise.resolve(jsonResponse(200, healthResponse([], healthPages === 1 ? oldSession : nextSession)));
      }
      if (input === '/api/layout') return Promise.resolve(jsonResponse(200, layoutReady()));
      throw new Error(`Unexpected ${input}`);
    });
    stubFetch(fetchMock);
    render(<App pollIntervalMs={1} />);
    await act(async () => { await vi.advanceTimersByTimeAsync(0); });
    await act(async () => { await vi.advanceTimersByTimeAsync(1); });

    const ui = within(document.body);
    expect(eventTargets).toEqual([
      '/api/events?after=0&limit=100',
      '/api/events?after=10&limit=100',
      '/api/events?after=0&limit=100',
    ]);
    expect(ui.getByRole('heading', { name: /Wearable 8/i })).not.toBeNull();
    expect(ui.queryByRole('heading', { name: /Wearable 7/i })).toBeNull();
    expect(ui.queryByRole('heading', { name: /Wearable 99/i })).toBeNull();
  });

  it('retries cursor zero when health adopts a new session before the old-cursor event request resolves', async () => {
    vi.useFakeTimers();
    vi.setSystemTime(new Date(0));
    const oldSession = '0123456789abcdef0123456789abcdef';
    const nextSession = 'fedcba9876543210fedcba9876543210';
    const delayedHealthB = deferred<Response>();
    const delayedEventsB = deferred<Response>();
    const eventTargets: string[] = [];
    let zeroPages = 0;
    let healthPages = 0;
    const eventsAt = (count: number, firstWearable: number) => Array.from(
      { length: count },
      (_, index) => eventRecord({
        cursor: index + 1,
        wearable: firstWearable + index,
        packet: (index + 1).toString(16).padStart(6, '0'),
        seq: index + 1,
        received_at_ms: 0,
      }),
    );
    const fetchMock = vi.fn((input: string) => {
      if (input.startsWith('/api/events')) {
        eventTargets.push(input);
        if (input.includes('after=10')) return delayedEventsB.promise;
        zeroPages += 1;
        if (zeroPages === 1) {
          return Promise.resolve(jsonResponse(200, eventsResponse(eventsAt(10, 7), false, oldSession)));
        }
        return Promise.resolve(jsonResponse(200, eventsResponse(eventsAt(5, 8), false, nextSession)));
      }
      if (input === '/api/health') {
        healthPages += 1;
        if (healthPages === 1) {
          return Promise.resolve(jsonResponse(200, { ...healthResponse([], oldSession), current_cursor: 10 }));
        }
        return delayedHealthB.promise;
      }
      if (input === '/api/layout') return Promise.resolve(jsonResponse(200, layoutReady()));
      throw new Error(`Unexpected ${input}`);
    });
    stubFetch(fetchMock);
    render(<App pollIntervalMs={1} />);

    await act(async () => { await vi.advanceTimersByTimeAsync(0); });
    await act(async () => { await vi.advanceTimersByTimeAsync(1); });
    expect(eventTargets).toEqual(['/api/events?after=0&limit=100', '/api/events?after=10&limit=100']);

    await act(async () => {
      delayedHealthB.resolve(jsonResponse(200, { ...healthResponse([], nextSession), current_cursor: 5 }));
      await vi.advanceTimersByTimeAsync(0);
    });
    await act(async () => {
      delayedEventsB.resolve(jsonResponse(200, eventsResponse(eventsAt(5, 99), false, nextSession)));
      await vi.advanceTimersByTimeAsync(0);
    });

    expect(eventTargets).toEqual([
      '/api/events?after=0&limit=100',
      '/api/events?after=10&limit=100',
      '/api/events?after=0&limit=100',
    ]);
    const ui = within(document.body);
    expect(ui.getByRole('heading', { name: /Wearable 8/i })).not.toBeNull();
    expect(ui.queryByRole('heading', { name: /Wearable 7/i })).toBeNull();
    expect(ui.queryByRole('heading', { name: /Wearable 99/i })).toBeNull();
  });

  it('ignores abort-insensitive responses from a cleaned-up poll effect before they can adopt a session', async () => {
    const oldSession = '0123456789abcdef0123456789abcdef';
    const nextSession = 'fedcba9876543210fedcba9876543210';
    const staleEvents = deferred<Response>();
    const staleHealth = deferred<Response>();
    const currentEvents = deferred<Response>();
    const currentHealth = deferred<Response>();
    let eventRequests = 0;
    let healthRequests = 0;
    const fetchMock = vi.fn((input: string) => {
      if (input.startsWith('/api/events')) {
        eventRequests += 1;
        return eventRequests === 1 ? staleEvents.promise : currentEvents.promise;
      }
      if (input === '/api/health') {
        healthRequests += 1;
        return healthRequests === 1 ? staleHealth.promise : currentHealth.promise;
      }
      if (input === '/api/layout') return Promise.resolve(jsonResponse(200, layoutReady()));
      throw new Error(`Unexpected ${input}`);
    });
    stubFetch(fetchMock);
    const view = render(<App pollIntervalMs={60_000} />);
    await waitFor(() => expect([eventRequests, healthRequests]).toEqual([1, 1]));
    view.rerender(<App pollIntervalMs={60_001} />);
    await waitFor(() => expect([eventRequests, healthRequests]).toEqual([2, 2]));

    await act(async () => {
      staleHealth.resolve(jsonResponse(200, gatewayHealth()));
      await Promise.resolve();
      await Promise.resolve();
    });
    const ui = within(document.body);
    expect(ui.queryByRole('switch', { name: 'Turn Gateway on' })).toBeNull();

    await act(async () => {
      staleEvents.resolve(jsonResponse(200, eventsResponse([
        eventRecord({ wearable: 7, packet: '000007', seq: 7 }),
      ], false, oldSession)));
      await Promise.resolve();
      await Promise.resolve();
    });
    expect(ui.queryByRole('heading', { name: /Wearable 7/i })).toBeNull();

    await act(async () => {
      currentEvents.resolve(jsonResponse(200, eventsResponse([
        eventRecord({ wearable: 8, packet: '000008', seq: 8 }),
      ], false, nextSession)));
      currentHealth.resolve(jsonResponse(200, healthResponse([
        healthDevice(0, { root: rootStatus({ role: 'leaf' }) }),
      ], nextSession)));
      await Promise.resolve();
      await Promise.resolve();
    });
    await ui.findByRole('switch', { name: 'Turn Gateway on' });
    expect(ui.getByRole('heading', { name: /Wearable 8/i })).not.toBeNull();
    expect(ui.queryByRole('heading', { name: /Wearable 7/i })).toBeNull();
  });

  it('keeps responsive, touch-sized, and reduced-motion floorplan rules in the stylesheet', async () => {
    const styles = await readFile('src/styles.css', 'utf8');
    expect(styles).toContain('.floorplan-content');
    expect(styles).toContain('.floorplan-node');
    expect(styles).toMatch(/\.floorplan-node-point \{[^}]*border-radius: 50%/);
    expect(styles).toMatch(/\.floorplan-marker-point \{[^}]*rotate\(45deg\)/);
    expect(styles).toContain('width: 44px; height: 44px');
    expect(styles).toContain('@media (max-width: 900px)');
    expect(styles).toContain('@media (max-width: 620px)');
    expect(styles).toContain('.node-table caption { display: block; width: 100%; }');
    expect(styles).toContain('prefers-reduced-motion: reduce');
  });

  it('recovers the Gateway control after a mismatched accepted command echo without exposing ROOT wording', async () => {
    const fetchMock = vi.fn((input: string) => {
      if (input.startsWith('/api/events')) return Promise.resolve(jsonResponse(200, eventsResponse()));
      if (input === '/api/health') return Promise.resolve(jsonResponse(200, gatewayHealth()));
      if (input === '/api/layout') return Promise.resolve(jsonResponse(200, layoutReady()));
      if (input === '/api/root') return Promise.resolve(jsonResponse(202, { schema: 'mind.command.v1', accepted: true, device: 1, command: 'on' }));
      throw new Error(`Unexpected ${input}`);
    });
    stubFetch(fetchMock);
    render(<App pollIntervalMs={60_000} />);
    const ui = within(document.body);
    const toggle = await ui.findByRole('switch', { name: 'Turn Gateway on' });
    fireEvent.click(toggle);
    await ui.findByText('Gateway command could not be completed. Retry Gateway.');
    expect(toggle).toHaveProperty('disabled', false);
    expect(document.body.textContent).not.toMatch(/root command/i);
  });

  it('keeps root HTTP and confirmation waits bounded and retryable', async () => {
    vi.useFakeTimers();
    const hungRootFetch = vi.fn((input: string, init?: RequestInit) => {
      if (input.startsWith('/api/events')) return Promise.resolve(jsonResponse(200, eventsResponse()));
      if (input === '/api/health') return Promise.resolve(jsonResponse(200, gatewayHealth()));
      if (input === '/api/layout') return Promise.resolve(jsonResponse(200, layoutReady()));
      if (input === '/api/root') {
        return new Promise<Response>((_resolve, reject) => {
          init?.signal?.addEventListener('abort', () => reject(new DOMException('Aborted', 'AbortError')));
        });
      }
      throw new Error(`Unexpected ${input}`);
    });
    stubFetch(hungRootFetch);
    render(<App pollIntervalMs={60_000} rootRequestTimeoutMs={5} rootConfirmationTimeoutMs={50} />);
    await act(async () => { await vi.advanceTimersByTimeAsync(0); });
    const ui = within(document.body);
    fireEvent.click(ui.getByRole('switch', { name: 'Turn Gateway on' }));
    await act(async () => { await vi.advanceTimersByTimeAsync(5); });
    expect(ui.getByRole('status', { name: 'Gateway command status' }).textContent).toContain('Request timed out; retry Gateway.');
    expect(ui.getByRole('switch', { name: 'Turn Gateway on' })).toHaveProperty('disabled', false);

    cleanup();
    stubFetch(bridgeFetch());
    render(<App pollIntervalMs={60_000} rootRequestTimeoutMs={50} rootConfirmationTimeoutMs={5} />);
    await act(async () => { await vi.advanceTimersByTimeAsync(0); });
    fireEvent.click(within(document.body).getByRole('switch', { name: 'Turn Gateway on' }));
    await act(async () => { await vi.advanceTimersByTimeAsync(5); });
    expect(within(document.body).getByRole('status', { name: 'Gateway command status' }).textContent).toContain('did not confirm the requested ON state in time');
    expect(within(document.body).getByRole('switch', { name: 'Turn Gateway on' })).toHaveProperty('disabled', false);
  });

  it('retains the gap warning and sanitizes bridge errors at Gateway and event presentation boundaries', async () => {
    stubFetch(bridgeFetch(healthResponse([])));
    const gapFetch = vi.fn((input: string, init?: RequestInit) => {
      if (input.startsWith('/api/events')) return Promise.resolve(jsonResponse(200, eventsResponse([eventRecord({ cursor: 2 })], true)));
      if (input === '/api/health') return Promise.resolve(jsonResponse(200, healthResponse([])));
      if (input === '/api/layout') return Promise.resolve(jsonResponse(200, layoutReady()));
      throw new Error(`Unexpected ${input}:${init?.method ?? 'GET'}`);
    });
    stubFetch(gapFetch);
    render(<App pollIntervalMs={60_000} />);
    await within(document.body).findByText(/Event history gap detected/i);
    cleanup();

    const errorFetch = vi.fn((input: string) => {
      if (input === '/api/layout') return Promise.resolve(jsonResponse(200, layoutReady()));
      return Promise.reject(new Error('ROOT STATUS transport failure'));
    });
    stubFetch(errorFetch);
    render(<App pollIntervalMs={60_000} />);
    const ui = within(document.body);
    await ui.findByText(/Bridge offline — retrying/i);
    expect(ui.getAllByRole('alert').map((alert) => alert.textContent).join(' ')).not.toMatch(/ROOT STATUS/i);
  });

  it('transitions to Ballpark at two seconds and recomputes late evidence without remounting its marker', async () => {
    vi.useFakeTimers();
    vi.setSystemTime(new Date(0));
    const observers = ['0102545678c1', '0102545678c2', '0102545678c3', '0102545678c4'];
    const initialEvents = observers.slice(0, 3).map((observer, index) => eventRecord({
      cursor: index + 1, observer, observer_rssi_dbm: -60, packet: '000007', seq: 7, received_at_ms: 0,
    }));
    const lateEvent = eventRecord({ cursor: 4, observer: observers[3], observer_rssi_dbm: -40, packet: '000007', seq: 7, received_at_ms: 3_000 });
    const layout = layoutReady({ positions: observers.map((adva, index) => ({
      adva,
      x: index === 1 || index === 3 ? 1 : 0,
      y: index > 1 ? 1 : 0,
    })) });
    let lateEnabled = false;
    let lateDelivered = false;
    stubFetch(vi.fn((input: string) => {
      if (input.startsWith('/api/events')) {
        if (input.includes('after=0')) return Promise.resolve(jsonResponse(200, {
          schema: 'mind.api.v2', session_id: '0123456789abcdef0123456789abcdef', gap: false, oldest_cursor: 1, current_cursor: 3, events: initialEvents,
        }));
        if (lateEnabled && !lateDelivered) {
          lateDelivered = true;
          return Promise.resolve(jsonResponse(200, {
            schema: 'mind.api.v2', session_id: '0123456789abcdef0123456789abcdef', gap: false, oldest_cursor: 1, current_cursor: 4, events: [lateEvent],
          }));
        }
        return Promise.resolve(jsonResponse(200, {
          schema: 'mind.api.v2', session_id: '0123456789abcdef0123456789abcdef', gap: false, oldest_cursor: 1, current_cursor: lateDelivered ? 4 : 3, events: [],
        }));
      }
      if (input === '/api/health') return Promise.resolve(jsonResponse(200, healthResponse([])));
      if (input === '/api/layout') return Promise.resolve(jsonResponse(200, layout));
      throw new Error(`Unexpected ${input}`);
    }));
    render(<App pollIntervalMs={1_000} />);
    const ui = within(document.body);
    await act(async () => { await vi.advanceTimersByTimeAsync(0); });
    expect(ui.getAllByText('Collecting').length).toBeGreaterThan(0);
    expect(ui.queryByRole('img', { name: /Wearable 7, packet 000007: Ballpark/i })).toBeNull();

    await act(async () => { await vi.advanceTimersByTimeAsync(2_000); });
    const marker = ui.getByRole('img', { name: /Wearable 7, packet 000007: Ballpark/i });
    const before = marker.getAttribute('aria-label');
    expect(ui.getAllByText('Ballpark').length).toBeGreaterThan(0);

    lateEnabled = true;
    await act(async () => { await vi.advanceTimersByTimeAsync(1_000); });
    const recomputedMarker = ui.getByRole('img', { name: /Wearable 7, packet 000007: Ballpark/i });
    expect(recomputedMarker).toBe(marker);
    expect(recomputedMarker.getAttribute('aria-label')).not.toBe(before);
  });

  it('schedules a collection transition at its exact deadline after an off-heartbeat event receipt', async () => {
    vi.useFakeTimers();
    vi.setSystemTime(new Date(0));
    const observers = ['0102545678c1', '0102545678c2', '0102545678c3'];
    const events = observers.map((observer, index) => eventRecord({
      cursor: index + 1, observer, observer_rssi_dbm: -55, packet: '000007', seq: 7, received_at_ms: 0,
    }));
    const layout = layoutReady({ positions: [
      { adva: observers[0], x: 0, y: 0 },
      { adva: observers[1], x: 1, y: 0 },
      { adva: observers[2], x: 0, y: 1 },
    ] });
    stubFetch(vi.fn((input: string) => {
      if (input.startsWith('/api/events')) {
        return new Promise<Response>((resolve) => {
          window.setTimeout(() => resolve(jsonResponse(200, eventsResponse(events))), 250);
        });
      }
      if (input === '/api/health') return Promise.resolve(jsonResponse(200, healthResponse([])));
      if (input === '/api/layout') return Promise.resolve(jsonResponse(200, layout));
      throw new Error(`Unexpected ${input}`);
    }));

    render(<App pollIntervalMs={60_000} />);
    const ui = within(document.body);
    await act(async () => { await vi.advanceTimersByTimeAsync(250); });
    expect(ui.getAllByText('Collecting').length).toBeGreaterThan(0);

    await act(async () => { await vi.advanceTimersByTimeAsync(1_749); });
    expect(ui.getAllByText('Collecting').length).toBeGreaterThan(0);
    expect(ui.queryByRole('img', { name: /Wearable 7, packet 000007: Ballpark/i })).toBeNull();

    await act(async () => { await vi.advanceTimersByTimeAsync(1); });
    expect(ui.getByRole('img', { name: /Wearable 7, packet 000007: Ballpark/i })).not.toBeNull();
  });

  it('removes a prior Ballpark marker when a current recovery read reports corrupt layout state', async () => {
    vi.useFakeTimers();
    vi.setSystemTime(new Date(0));
    const local = '1842de524add';
    const observers = ['0102545678c1', '0102545678c2', '0102545678c3'];
    const events = observers.map((observer, index) => eventRecord({
      cursor: index + 1, observer, observer_rssi_dbm: -55, packet: '000007', seq: 7, received_at_ms: 0,
    }));
    const layout = layoutReady({ positions: [
      { adva: observers[0], x: 0, y: 0 },
      { adva: observers[1], x: 1, y: 0 },
      { adva: observers[2], x: 0, y: 1 },
      { adva: local, x: 1, y: 1 },
    ] });
    const corrupt = {
      schema: 'mind.dashboard.layout.v1', status: 'corrupt', error: 'corrupt_state', revision: 1, floorplan: null, positions: [],
    } as const;
    const health = healthResponse([healthDevice(0, {
      root: rootStatus({ cursor: 3, local }),
      gtt: gttSnapshot({ local, entries: [
        gttEntry({ index: 0, adva: local }),
        ...observers.map((adva, index) => gttEntry({ index: index + 1, adva })),
      ] }),
    })]);
    let layoutReads = 0;
    stubFetch(vi.fn((input: string, init?: RequestInit) => {
      if (input.startsWith('/api/events')) return Promise.resolve(jsonResponse(200, eventsResponse(events)));
      if (input === '/api/health') return Promise.resolve(jsonResponse(200, health));
      if (input === '/api/layout' && !init?.method) {
        return Promise.resolve(jsonResponse(200, layoutReads++ === 0 ? layout : corrupt));
      }
      if (input === '/api/layout' && init?.method === 'PUT') {
        return Promise.resolve(jsonResponse(503, { schema: 'mind.dashboard.error.v1', accepted: false, error: 'storage_unavailable' }));
      }
      throw new Error(`Unexpected ${input}`);
    }));

    render(<App pollIntervalMs={60_000} />);
    const ui = within(document.body);
    await act(async () => { await vi.advanceTimersByTimeAsync(2_000); });
    expect(ui.getByRole('img', { name: /Wearable 7, packet 000007: Ballpark/i })).not.toBeNull();

    const unplace = ui.getAllByRole('button', { name: 'Unplace' })[0]!;
    expect(unplace).toHaveProperty('disabled', false);
    fireEvent.click(unplace);
    await act(async () => { await vi.advanceTimersByTimeAsync(0); });
    expect(ui.getByText('Floorplan state is corrupt. Recover storage before editing.')).not.toBeNull();
    expect(ui.queryByRole('img', { name: /Wearable 7, packet 000007: Ballpark/i })).toBeNull();
  });

  it('keeps persisted Ballpark views visible while a mismatched GTT disables placement', async () => {
    vi.useFakeTimers();
    vi.setSystemTime(new Date(0));
    const observers = ['0102545678c1', '0102545678c2', '0102545678c3'];
    const events = observers.map((observer, index) => eventRecord({
      cursor: index + 1, observer, observer_rssi_dbm: -55, packet: '000007', seq: 7, received_at_ms: 0,
    }));
    const mismatchedHealth = healthResponse([healthDevice(0, {
      root: rootStatus({ local: '1842de524add' }),
      gtt: gttSnapshot({
        local: '1842de524add',
        entries: [gttEntry({ adva: '0102545678ff' })],
      }),
    })]);
    stubFetch(vi.fn((input: string) => {
      if (input.startsWith('/api/events')) return Promise.resolve(jsonResponse(200, {
        schema: 'mind.api.v2', session_id: '0123456789abcdef0123456789abcdef', gap: false, oldest_cursor: 1, current_cursor: 3, events: input.includes('after=0') ? events : [],
      }));
      if (input === '/api/health') return Promise.resolve(jsonResponse(200, mismatchedHealth));
      if (input === '/api/layout') return Promise.resolve(jsonResponse(200, layoutReady({ positions: [
        { adva: observers[0], x: 0, y: 0 }, { adva: observers[1], x: 1, y: 0 }, { adva: observers[2], x: 0, y: 1 },
      ] })));
      if (input === '/api/gtt') return Promise.resolve(jsonResponse(202, { schema: 'mind.command.v1', accepted: true, device: 0, command: 'gtt' }));
      throw new Error(`Unexpected ${input}`);
    }));
    render(<App pollIntervalMs={60_000} />);
    const ui = within(document.body);
    await act(async () => { await vi.advanceTimersByTimeAsync(2_000); });
    expect(ui.getByRole('img', { name: /Wearable 7, packet 000007: Ballpark/i })).not.toBeNull();
    expect(ui.getByText('Recent incident location states')).not.toBeNull();
    expect(document.querySelector('.floorplan-content')).not.toBeNull();
    expect(ui.queryByRole('button', { name: 'Place at center' })).toBeNull();
    expect(ui.queryByText('Unpositioned nodes')).toBeNull();
  });

  it('caps the first ten feed-order views before filtering map markers', async () => {
    vi.useFakeTimers();
    vi.setSystemTime(new Date(0));
    const observers = ['0102545678c1', '0102545678c2', '0102545678c3'];
    let cursor = 1;
    const records = Array.from({ length: 11 }, (_, wearableIndex) => {
      const wearable = wearableIndex + 1;
      const packet = wearable.toString(16).padStart(6, '0');
      const evidenceCount = wearable === 11 ? 2 : 3;
      return observers.slice(0, evidenceCount).map((observer) => eventRecord({
        cursor: cursor++, wearable, packet, seq: wearable, observer, observer_rssi_dbm: -55, received_at_ms: 0,
      }));
    }).flat();
    stubFetch(vi.fn((input: string) => {
      if (input.startsWith('/api/events')) return Promise.resolve(jsonResponse(200, {
        schema: 'mind.api.v2', session_id: '0123456789abcdef0123456789abcdef', gap: false, oldest_cursor: 1, current_cursor: records.length, events: input.includes('after=0') ? records : [],
      }));
      if (input === '/api/health') return Promise.resolve(jsonResponse(200, healthResponse([])));
      if (input === '/api/layout') return Promise.resolve(jsonResponse(200, layoutReady({ positions: [
        { adva: observers[0], x: 0, y: 0 }, { adva: observers[1], x: 1, y: 0 }, { adva: observers[2], x: 0, y: 1 },
      ] })));
      throw new Error(`Unexpected ${input}`);
    }));
    render(<App pollIntervalMs={60_000} />);
    await act(async () => { await vi.advanceTimersByTimeAsync(2_000); });
    const ui = within(document.body);
    expect(ui.getByText('11 logical events')).not.toBeNull();
    expect(document.querySelectorAll('.estimate-table tbody tr')).toHaveLength(10);
    expect(ui.getAllByRole('img', { name: /Ballpark/i })).toHaveLength(9);
    expect(document.body.textContent).not.toMatch(/estimated|invalid_input|meters|precision/i);
  });
});
