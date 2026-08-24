import { readFile } from 'node:fs/promises';
import './test/runtime';
import { act, cleanup, fireEvent, render, waitFor, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { afterEach, describe, expect, it, vi } from 'vitest';
import App from './App';
import { Header } from './components/Header';
import { eventRecord, eventsResponse, healthDevice, healthResponse, jsonResponse, rootStatus } from './test/fixtures';
import { restoreFetch, stubFetch } from './test/runtime';

afterEach(() => {
  cleanup();
  restoreFetch();
  vi.useRealTimers();
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
    if (input === '/api/gtt') {
      const request = JSON.parse(String(init?.body)) as { device: number };
      return Promise.resolve(jsonResponse(202, {
        schema: 'mind.command.v1', accepted: true, device: request.device, command: 'gtt',
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

  it('keeps semantic landmarks and human wearable identity while placing topology between root devices and wearable state', async () => {
    const health = healthResponse([healthDevice(0, { root: rootStatus({ role: 'leaf' }) })]);
    stubFetch(bridgeFetch(undefined, health));
    render(<App pollIntervalMs={60_000} />);

    await ui().findByRole('switch', { name: 'Turn root on for Node 1' });
    expect(ui().getByRole('banner')).not.toBeNull();
    expect(ui().getByRole('main')).not.toBeNull();
    expect(ui().getByRole('heading', { name: 'Root devices' })).not.toBeNull();
    expect(ui().getByRole('heading', { name: 'Topology' })).not.toBeNull();
    expect(ui().getByRole('heading', { name: 'Wearable state' })).not.toBeNull();
    expect(ui().getByRole('heading', { name: 'Event feed' })).not.toBeNull();

    const headings = [...document.querySelectorAll('.primary-column h2')].map((heading) => heading.textContent);
    expect(headings).toEqual(['Root devices', 'Topology', 'Wearable state']);
    const article = ui().getByRole('heading', { name: /Wearable 7 — Confirmed fall/ }).closest('article');
    expect(article?.textContent?.indexOf('Wearable 7')).toBeLessThan(article?.textContent?.indexOf('Packet ID 00002a') ?? Infinity);
  });

  it('uses the root switch to send only the opposite state and keeps it pending after HTTP 202', async () => {
    const health = healthResponse([healthDevice(0, { root: rootStatus({ role: 'leaf' }) })]);
    const fetchMock = bridgeFetch(eventsResponse(), health);
    stubFetch(fetchMock);
    const user = userEvent.setup();
    render(<App pollIntervalMs={60_000} />);

    const rootSwitch = await ui().findByRole('switch', { name: 'Turn root on for Node 1' });
    expect(rootSwitch.getAttribute('aria-checked')).toBe('false');
    await user.click(rootSwitch);
    await waitFor(() => expect(fetchMock).toHaveBeenCalledWith('/api/root', expect.objectContaining({ body: '{"device":0,"active":true}' })));
    await waitFor(() => expect(ui().getByRole('status', { name: 'Root command status' }).textContent).toContain('accepted serial write'));
    expect(rootSwitch).toHaveProperty('disabled', true);
    await user.click(rootSwitch);
    expect(fetchMock.mock.calls.filter(([input]) => input === '/api/root')).toHaveLength(1);
    expect(ui().getByText(/Requested ROOT ON; awaiting bridge confirmation/i)).not.toBeNull();
  });

  it('recovers a root switch after a mismatched accepted echo', async () => {
    const health = healthResponse([healthDevice(0, { root: rootStatus({ role: 'leaf' }) })]);
    const fetchMock = vi.fn((input: string) => {
      if (input.startsWith('/api/events')) return Promise.resolve(jsonResponse(200, eventsResponse()));
      if (input === '/api/health') return Promise.resolve(jsonResponse(200, health));
      if (input === '/api/gtt') return Promise.resolve(jsonResponse(202, { schema: 'mind.command.v1', accepted: true, device: 0, command: 'gtt' }));
      if (input === '/api/root') return Promise.resolve(jsonResponse(202, { schema: 'mind.command.v1', accepted: true, device: 1, command: 'on' }));
      throw new Error(`Unexpected request ${input}`);
    });
    stubFetch(fetchMock);
    const user = userEvent.setup();
    render(<App pollIntervalMs={60_000} />);

    const rootSwitch = await ui().findByRole('switch', { name: 'Turn root on for Node 1' });
    await user.click(rootSwitch);
    await waitFor(() => expect(ui().getByRole('status', { name: 'Root command status' }).textContent).toContain('did not echo'));
    expect(rootSwitch).toHaveProperty('disabled', false);
  });

  it('keeps bounded HTTP and authoritative-confirmation timeouts separate and recoverable', async () => {
    vi.useFakeTimers();
    const health = healthResponse([healthDevice(0, { root: rootStatus({ role: 'leaf' }) })]);
    const hungRootFetch = vi.fn((input: string, init?: RequestInit) => {
      if (input.startsWith('/api/events')) return Promise.resolve(jsonResponse(200, eventsResponse()));
      if (input === '/api/health') return Promise.resolve(jsonResponse(200, health));
      if (input === '/api/gtt') return Promise.resolve(jsonResponse(202, { schema: 'mind.command.v1', accepted: true, device: 0, command: 'gtt' }));
      if (input === '/api/root') {
        return new Promise<Response>((_resolve, reject) => {
          init?.signal?.addEventListener('abort', () => reject(new DOMException('Aborted', 'AbortError')));
        });
      }
      throw new Error(`Unexpected request ${input}`);
    });
    stubFetch(hungRootFetch);
    render(<App pollIntervalMs={60_000} rootRequestTimeoutMs={5} rootConfirmationTimeoutMs={50} />);
    const ui = within(document.body);
    await act(async () => { await vi.advanceTimersByTimeAsync(0); });
    const rootSwitch = ui.getByRole('switch', { name: 'Turn root on for Node 1' });
    fireEvent.click(rootSwitch);
    await act(async () => { await vi.advanceTimersByTimeAsync(5); });
    expect(ui.getByRole('status', { name: 'Root command status' }).textContent).toContain('Request timed out; retry this device.');
    expect(rootSwitch).toHaveProperty('disabled', false);

    cleanup();
    const acceptedRootFetch = bridgeFetch(eventsResponse(), health);
    stubFetch(acceptedRootFetch);
    render(<App pollIntervalMs={60_000} rootRequestTimeoutMs={50} rootConfirmationTimeoutMs={5} />);
    await act(async () => { await vi.advanceTimersByTimeAsync(0); });
    const confirmationSwitch = within(document.body).getByRole('switch', { name: 'Turn root on for Node 1' });
    fireEvent.click(confirmationSwitch);
    await act(async () => { await vi.advanceTimersByTimeAsync(5); });
    expect(within(document.body).getByRole('status', { name: 'Root command status' }).textContent).toContain('did not publish the requested ROOT ON state in time');
    expect(confirmationSwitch).toHaveProperty('disabled', false);
  });

  it('shows gap, empty, and recoverable bridge error states without dropping landmarks', async () => {
    stubFetch(bridgeFetch(eventsResponse([eventRecord({ cursor: 2 })], true), healthResponse([])));
    render(<App pollIntervalMs={60_000} />);
    await ui().findByText(/Event history gap detected/i);
    expect(ui().getByText(/No configured serial devices/i)).not.toBeNull();
    expect(ui().getByText(/No connected physical device is eligible/i)).not.toBeNull();

    const errorFetch = vi.fn((input: string) => Promise.resolve(jsonResponse(503, {
      schema: 'mind.error.v1', accepted: false, error: input.includes('health') ? 'disconnected' : 'write_failed',
    })));
    stubFetch(errorFetch);
    render(<App pollIntervalMs={60_000} />);
    await ui().findAllByRole('alert');
    expect(ui().getByText(/Bridge offline — retrying/i)).not.toBeNull();
  });

  it('keeps compact responsive grids, touch targets, and reduced-motion support in the stylesheet', async () => {
    const styles = await readFile('src/styles.css', 'utf8');
    expect(styles).toContain('grid-template-columns: minmax(0, 2fr) minmax(21rem, 1fr)');
    expect(styles).toContain('.compact-card');
    expect(styles).toContain('border-left: 4px solid');
    expect(styles).toContain('@media (max-width: 900px)');
    expect(styles).toContain('@media (max-width: 620px)');
    expect(styles).toContain('.topology-grid, .wearable-grid { grid-template-columns: 1fr; }');
    expect(styles).toContain('.device-table { min-width: 0; display: block; }');
    expect(styles).toContain('min-height: 44px');
    expect(styles).toContain('prefers-reduced-motion: reduce');
  });
});
