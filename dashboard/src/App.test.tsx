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

describe('Gateway floorplan dashboard', () => {
  it('renders one Gateway, a focal floorplan, staged GTT nodes, and no visible ROOT vocabulary', async () => {
    stubFetch(bridgeFetch());
    render(<App pollIntervalMs={60_000} />);
    const ui = within(document.body);
    await ui.findByRole('heading', { name: 'Floorplan' });
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

  it('keeps responsive, touch-sized, and reduced-motion floorplan rules in the stylesheet', async () => {
    const styles = await readFile('src/styles.css', 'utf8');
    expect(styles).toContain('.floorplan-content');
    expect(styles).toContain('.floorplan-node');
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
});
