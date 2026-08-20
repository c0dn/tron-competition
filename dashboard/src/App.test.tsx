import { readFile } from 'node:fs/promises';
import './test/runtime';
import { cleanup, render, waitFor, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { afterEach, describe, expect, it, vi } from 'vitest';
import App from './App';
import { Header } from './components/Header';
import { RootControlPanel } from './components/RootControlPanel';
import { eventRecord, eventsResponse, healthDevice, healthResponse, jsonResponse, rootStatus } from './test/fixtures';
import { restoreFetch, stubFetch } from './test/runtime';

afterEach(() => {
  cleanup();
  restoreFetch();
});

function ui() {
  return within(document.body);
}

function bridgeFetch(page = eventsResponse([eventRecord()]), health = healthResponse()) {
  return vi.fn((input: string, init?: RequestInit) => {
    if (input.startsWith('/api/events')) return Promise.resolve(jsonResponse(200, page));
    if (input === '/api/health') return Promise.resolve(jsonResponse(200, health));
    if (input === '/api/root') {
      const request = JSON.parse(String(init?.body)) as { device: number; active: boolean };
      return Promise.resolve(jsonResponse(202, {
        schema: 'mind.command.v1', accepted: true, device: request.device, command: request.active ? 'on' : 'off',
      }));
    }
    throw new Error(`Unexpected request ${input}`);
  });
}

describe('operations dashboard', () => {
  it('separates the bridge label and status message in the status region', () => {
    render(<Header status={{ kind: 'online', message: 'Bridge receiving records' }} />);
    const status = ui().getByRole('status', { name: 'Bridge: Bridge receiving records' });
    expect(status.querySelector('strong')?.textContent).toBe('Bridge:');
    expect(status.querySelector('span')?.textContent).toBe('Bridge receiving records');
  });

  it('uses semantic landmarks and leads event/wearable output with the human wearable key', async () => {
    stubFetch(bridgeFetch());
    render(<App pollIntervalMs={60_000} />);

    await ui().findByRole('rowheader', { name: /Device 0/ });
    expect(ui().getByRole('banner')).not.toBeNull();
    expect(ui().getByRole('main')).not.toBeNull();
    expect(ui().getByRole('heading', { name: 'Root controls' })).not.toBeNull();
    expect(ui().getByRole('heading', { name: 'Wearable state' })).not.toBeNull();
    expect(ui().getByRole('heading', { name: 'Event feed' })).not.toBeNull();
    expect(ui().getByRole('table', { name: /configured serial devices/i })).not.toBeNull();
    expect(ui().getAllByRole('list').length).toBeGreaterThanOrEqual(2);

    const article = ui().getByRole('heading', { name: /Wearable 7 — Confirmed fall/ }).closest('article');
    expect(article).not.toBeNull();
    const text = article?.textContent ?? '';
    expect(text.indexOf('Wearable 7')).toBeLessThan(text.indexOf('Packet ID 00002a'));
  });

  it('renders every configured device without a UI cap and only disables the pending device', async () => {
    const devices = Array.from({ length: 16 }, (_, device) => healthDevice(device));
    let resolveRoot: (() => void) | undefined;
    const fetchMock = vi.fn((input: string, init?: RequestInit) => {
      if (input.startsWith('/api/events')) return Promise.resolve(jsonResponse(200, eventsResponse()));
      if (input === '/api/health') return Promise.resolve(jsonResponse(200, healthResponse(devices)));
      if (input === '/api/root') {
        const request = JSON.parse(String(init?.body)) as { device: number; active: boolean };
        return new Promise<Response>((resolve) => {
          resolveRoot = () => resolve(jsonResponse(202, {
            schema: 'mind.command.v1', accepted: true, device: request.device, command: request.active ? 'on' : 'off',
          }));
        });
      }
      throw new Error(`Unexpected request ${input}`);
    });
    stubFetch(fetchMock);
    const user = userEvent.setup();
    render(<App pollIntervalMs={60_000} />);

    await ui().findByRole('rowheader', { name: /Device 15/ });
    expect(ui().getByRole('rowheader', { name: /Device 15/ })).not.toBeNull();
    const device0 = ui().getByRole('rowheader', { name: /Device 0/ }).closest('tr');
    const device1 = ui().getByRole('rowheader', { name: /^Device 1\b/ }).closest('tr');
    if (!device0 || !device1) throw new Error('Expected device rows.');
    const on0 = within(device0).getByRole('button', { name: 'Request ROOT ON for Device 0' });
    const on1 = within(device1).getByRole('button', { name: 'Request ROOT ON for Device 1' });
    on0.focus();
    await user.keyboard('{Enter}');

    expect(on0).toHaveProperty('disabled', true);
    expect(on1).toHaveProperty('disabled', false);
    expect(fetchMock).toHaveBeenLastCalledWith('/api/root', expect.objectContaining({ body: '{"device":0,"active":true}' }));
    if (!resolveRoot) throw new Error('Expected pending root request.');
    resolveRoot();
    await waitFor(() => expect(ui().getByRole('status', { name: 'Root command status' }).textContent).toContain('accepted ROOT ON'));
  });

  it('recovers the original control when an accepted root response echoes the wrong device', async () => {
    const fetchMock = vi.fn((input: string) => {
      if (input.startsWith('/api/events')) return Promise.resolve(jsonResponse(200, eventsResponse()));
      if (input === '/api/health') return Promise.resolve(jsonResponse(200, healthResponse()));
      if (input === '/api/root') {
        return Promise.resolve(jsonResponse(202, {
          schema: 'mind.command.v1', accepted: true, device: 1, command: 'on',
        }));
      }
      throw new Error(`Unexpected request ${input}`);
    });
    stubFetch(fetchMock);
    const user = userEvent.setup();
    render(<App pollIntervalMs={60_000} />);

    const row = (await ui().findByRole('rowheader', { name: /Device 0/ })).closest('tr');
    if (!row) throw new Error('Expected Device 0 row.');
    const on = within(row).getByRole('button', { name: 'Request ROOT ON for Device 0' });
    await user.click(on);
    await waitFor(() => expect(ui().getByRole('status', { name: 'Root command status' }).textContent).toContain('root command failed'));
    expect(ui().getByRole('status', { name: 'Root command status' }).textContent).toContain('did not echo');
    expect(on).toHaveProperty('disabled', false);
  });

  it('aborts a timed-out root request, announces recovery, and leaves other controls available', async () => {
    const devices = [healthDevice(0), healthDevice(1)];
    let aborted = false;
    const fetchMock = vi.fn((input: string, init?: RequestInit) => {
      if (input.startsWith('/api/events')) return Promise.resolve(jsonResponse(200, eventsResponse()));
      if (input === '/api/health') return Promise.resolve(jsonResponse(200, healthResponse(devices)));
      if (input === '/api/root') {
        return new Promise<Response>((_resolve, reject) => {
          init?.signal?.addEventListener('abort', () => {
            aborted = true;
            reject(new DOMException('Aborted', 'AbortError'));
          });
        });
      }
      throw new Error(`Unexpected request ${input}`);
    });
    stubFetch(fetchMock);
    const user = userEvent.setup();
    render(<App pollIntervalMs={60_000} rootRequestTimeoutMs={250} />);

    const device0 = (await ui().findByRole('rowheader', { name: /Device 0/ })).closest('tr');
    const device1 = ui().getByRole('rowheader', { name: /^Device 1\b/ }).closest('tr');
    if (!device0 || !device1) throw new Error('Expected device rows.');
    const on0 = within(device0).getByRole('button', { name: 'Request ROOT ON for Device 0' });
    const on1 = within(device1).getByRole('button', { name: 'Request ROOT ON for Device 1' });
    await user.click(on0);
    expect(on0).toHaveProperty('disabled', true);
    expect(on1).toHaveProperty('disabled', false);
    await waitFor(() => expect(ui().getByRole('status', { name: 'Root command status' }).textContent).toContain('Request timed out; retry this device.'));
    expect(aborted).toBe(true);
    expect(on0).toHaveProperty('disabled', false);
  });

  it('immediately refetches from zero after a cursor epoch reset and accepts lower post-reset cursors', async () => {
    const calls: string[] = [];
    const prior = eventRecord({ cursor: 5, wearable: 7, packet: '00002a', seq: 42 });
    const restarted = eventRecord({ cursor: 1, wearable: 8, packet: '000001', seq: 1 });
    let zeroRequests = 0;
    const fetchMock = vi.fn((input: string) => {
      if (input.startsWith('/api/events')) {
        calls.push(input);
        if (input.includes('after=0')) {
          zeroRequests += 1;
          return Promise.resolve(jsonResponse(200, zeroRequests === 1
            ? eventsResponse([prior], true)
            : eventsResponse([restarted])));
        }
        if (input.includes('after=5')) {
          return Promise.resolve(jsonResponse(200, {
            schema: 'mind.api.v1', gap: false, oldest_cursor: 1, current_cursor: 1, events: [],
          }));
        }
      }
      if (input === '/api/health') return Promise.resolve(jsonResponse(200, healthResponse()));
      throw new Error(`Unexpected request ${input}`);
    });
    stubFetch(fetchMock);
    render(<App pollIntervalMs={10} />);

    await ui().findByRole('heading', { name: 'Wearable 8' });
    expect(calls).toContain('/api/events?after=5&limit=100');
    expect(calls.filter((call) => call === '/api/events?after=0&limit=100')).toHaveLength(2);
    expect(ui().getByRole('heading', { name: 'Wearable 7' })).not.toBeNull();
  });

  it('keeps a new-epoch stream root when delayed health started before the reset, then accepts fresh health', async () => {
    const oldRoot = { cursor: 5, device: 0, ...rootStatus({ role: 'leaf' }) };
    const restartedRoot = { cursor: 1, device: 0, ...rootStatus({ role: 'root' }) };
    const oldHealth = {
      ...healthResponse([healthDevice(0, { root: rootStatus({ role: 'leaf' }) })]),
      oldest_cursor: 5,
      current_cursor: 5,
    };
    const freshHealth = {
      ...healthResponse([healthDevice(0, { root: rootStatus({ role: 'leaf' }) })]),
      oldest_cursor: 1,
      current_cursor: 1,
    };
    let zeroRequests = 0;
    let healthRequests = 0;
    let resolveOldHealth: ((response: Response) => void) | undefined;
    let resolveFreshHealth: ((response: Response) => void) | undefined;
    const fetchMock = vi.fn((input: string) => {
      if (input === '/api/events?after=0&limit=100') {
        zeroRequests += 1;
        return Promise.resolve(jsonResponse(200, zeroRequests === 1
          ? eventsResponse([oldRoot], true)
          : eventsResponse([restartedRoot])));
      }
      if (input === '/api/events?after=5&limit=100') {
        return Promise.resolve(jsonResponse(200, {
          schema: 'mind.api.v1', gap: false, oldest_cursor: 1, current_cursor: 1, events: [],
        }));
      }
      if (input === '/api/events?after=1&limit=100') {
        return Promise.resolve(jsonResponse(200, {
          schema: 'mind.api.v1', gap: false, oldest_cursor: 1, current_cursor: 1, events: [],
        }));
      }
      if (input === '/api/health') {
        healthRequests += 1;
        if (healthRequests === 1) return Promise.resolve(jsonResponse(200, oldHealth));
        if (healthRequests === 2) {
          return new Promise<Response>((resolve) => {
            resolveOldHealth = resolve;
          });
        }
        return new Promise<Response>((resolve) => {
          resolveFreshHealth = resolve;
        });
      }
      throw new Error(`Unexpected request ${input}`);
    });
    stubFetch(fetchMock);
    render(<App pollIntervalMs={10} />);

    await ui().findByText('Runtime root');
    if (!resolveOldHealth) throw new Error('Expected old-epoch health request.');
    resolveOldHealth(jsonResponse(200, oldHealth));
    await waitFor(() => expect(healthRequests).toBe(3));
    expect(ui().getByText('Runtime root')).not.toBeNull();

    if (!resolveFreshHealth) throw new Error('Expected fresh health request.');
    resolveFreshHealth(jsonResponse(200, freshHealth));
    await waitFor(() => expect(ui().getByText('Leaf')).not.toBeNull());
  });

  it('drains a full 100-record page before advancing the polling cursor', async () => {
    const hundred = Array.from({ length: 100 }, (_, index) => {
      const cursor = index + 1;
      return eventRecord({
        cursor,
        wearable: (index % 254) + 1,
        packet: cursor.toString(16).padStart(6, '0'),
        seq: cursor & 0xff,
      });
    });
    const final = eventRecord({ cursor: 101, wearable: 8, packet: '000065', seq: 101 });
    const fetchMock = vi.fn((input: string) => {
      if (input === '/api/events?after=0&limit=100') {
        return Promise.resolve(jsonResponse(200, { ...eventsResponse(hundred), current_cursor: 101 }));
      }
      if (input === '/api/events?after=100&limit=100') return Promise.resolve(jsonResponse(200, eventsResponse([final])));
      if (input === '/api/health') {
        return Promise.resolve(jsonResponse(200, { ...healthResponse(), oldest_cursor: 1, current_cursor: 101 }));
      }
      throw new Error(`Unexpected request ${input}`);
    });
    stubFetch(fetchMock);
    render(<App pollIntervalMs={60_000} />);

    await ui().findByRole('heading', { name: 'Wearable 8' });
    expect(fetchMock).toHaveBeenCalledWith('/api/events?after=100&limit=100', expect.anything());
  });

  it('uses fresh health root/null only within its epoch and otherwise keeps new stream root authority', () => {
    const streamRoot = { cursor: 5, device: 0, ...rootStatus({ role: 'root' }) };
    const common = {
      commandRecords: {},
      pendingRoot: {},
      announcement: '',
      onSetRoot: () => undefined,
    };
    render(
      <RootControlPanel
        devices={[healthDevice(0, { root: null })]}
        healthCurrentCursor={5}
        healthEpoch={0}
        currentEpoch={0}
        rootRecords={{ 0: streamRoot }}
        {...common}
      />,
    );
    expect(ui().getByText('No root record yet')).not.toBeNull();
    cleanup();

    render(
      <RootControlPanel
        devices={[healthDevice(0, { root: rootStatus({ role: 'leaf' }) })]}
        healthCurrentCursor={5}
        healthEpoch={0}
        currentEpoch={0}
        rootRecords={{ 0: streamRoot }}
        {...common}
      />,
    );
    expect(ui().getByText('Leaf')).not.toBeNull();
    cleanup();

    render(
      <RootControlPanel
        devices={[healthDevice(0, { root: rootStatus({ role: 'leaf' }) })]}
        healthCurrentCursor={4}
        healthEpoch={0}
        currentEpoch={0}
        rootRecords={{ 0: streamRoot }}
        {...common}
      />,
    );
    expect(ui().getByText('Runtime root')).not.toBeNull();
    cleanup();

    render(
      <RootControlPanel
        devices={[healthDevice(0, { root: rootStatus({ role: 'leaf' }) })]}
        healthCurrentCursor={5}
        healthEpoch={0}
        currentEpoch={1}
        rootRecords={{ 0: { ...streamRoot, cursor: 1 } }}
        healthError="bridge unavailable"
        {...common}
      />,
    );
    expect(ui().getByText('Runtime root')).not.toBeNull();
    expect(ui().getByText(/Health request failed: bridge unavailable/i)).not.toBeNull();
    cleanup();

    render(
      <RootControlPanel
        devices={[healthDevice(0, { root: rootStatus({ role: 'leaf' }) })]}
        healthCurrentCursor={1}
        healthEpoch={1}
        currentEpoch={1}
        rootRecords={{ 0: { ...streamRoot, cursor: 1 } }}
        {...common}
      />,
    );
    expect(ui().getByText('Leaf')).not.toBeNull();
  });

  it('shows gap, empty, and recoverable error states', async () => {
    stubFetch(bridgeFetch(eventsResponse([eventRecord({ cursor: 2 })], true), healthResponse([])));
    render(<App pollIntervalMs={60_000} />);
    await ui().findByText(/Event history gap detected/i);
    expect(ui().getByText(/No configured serial devices/i)).not.toBeNull();
    expect(ui().getAllByRole('heading', { name: /Wearable 7/ }).length).toBe(2);

    const errorFetch = vi.fn((input: string) => Promise.resolve(jsonResponse(503, {
      schema: 'mind.error.v1', accepted: false, error: input.includes('health') ? 'disconnected' : 'write_failed',
    })));
    stubFetch(errorFetch);
    render(<App pollIntervalMs={60_000} />);
    await ui().findAllByRole('alert');
    expect(ui().getByText(/Bridge offline — retrying/i)).not.toBeNull();
  });

  it('keeps responsive reflow, touch-sized controls, and reduced-motion support in the stylesheet', async () => {
    const styles = await readFile('src/styles.css', 'utf8');
    expect(styles).toContain('@media (max-width: 900px)');
    expect(styles).toContain('@media (max-width: 620px)');
    expect(styles).toContain('.device-table { min-width: 0; display: block; }');
    expect(styles).toContain('.device-table td[data-label]::before');
    expect(styles).toContain('.table-wrap { overflow: visible; }');
    expect(styles).toContain('min-height: 44px');
    expect(styles).toContain('prefers-reduced-motion: reduce');
  });
});
