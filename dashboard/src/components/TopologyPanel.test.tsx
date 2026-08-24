import '../test/runtime';
import type { ComponentProps } from 'react';
import { act, cleanup, fireEvent, render, waitFor, within } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { TopologyPanel, groupGttEntries, isGttStale } from './TopologyPanel';
import { eligibleTopologySources, rootRequestBaseline } from '../state/devices';
import { gttEntry, gttSnapshot, healthDevice, jsonResponse, rootStatus } from '../test/fixtures';
import { restoreFetch, stubFetch } from '../test/runtime';

afterEach(() => {
  cleanup();
  restoreFetch();
  vi.useRealTimers();
});

function topology(devices = [healthDevice(0)], overrides: Partial<ComponentProps<typeof TopologyPanel>> = {}) {
  return (
    <TopologyPanel
      devices={devices}
      healthEpoch={0}
      currentEpoch={0}
      healthCurrentCursor={0}
      rootRecords={{}}
      now={20_000}
      {...overrides}
    />
  );
}

function acceptedGttFetch() {
  return vi.fn((input: string, init?: RequestInit) => {
    if (input !== '/api/gtt') throw new Error(`Unexpected request ${input}`);
    const request = JSON.parse(String(init?.body)) as { device: number };
    return Promise.resolve(jsonResponse(202, { schema: 'mind.command.v1', accepted: true, device: request.device, command: 'gtt' }));
  });
}

describe('topology source and GTT presentation', () => {
  it('uses only connected physical owners, switching from all owners to authoritative active roots', () => {
    const devices = [
      healthDevice(2),
      healthDevice(0),
      healthDevice(1, { owner_device: 0 }),
      healthDevice(3, { connected: false }),
    ];
    expect(eligibleTopologySources(devices, 0, 0, 0, {})).toMatchObject([{ device: 0 }, { device: 2 }]);

    const roots = [
      healthDevice(0, { root: rootStatus({ node: 1, role: 'root' }) }),
      healthDevice(1, { owner_device: 0, root: rootStatus({ node: 1, role: 'root' }) }),
      healthDevice(2, { root: rootStatus({ node: 2, role: 'leaf' }) }),
      healthDevice(4, { root: rootStatus({ node: 3, role: 'root' }) }),
    ];
    expect(eligibleTopologySources(roots, 0, 0, 1, {})).toMatchObject([{ device: 0 }, { device: 4 }]);
  });

  it('does not select a null-health device as an active-root topology source from a stale stream root', () => {
    const staleRoot = { device: 0, ...rootStatus({ role: 'root', cursor: 5 }) };
    const currentRoot = healthDevice(1, { root: rootStatus({ node: 2, role: 'root', cursor: 10 }) });

    expect(eligibleTopologySources([
      healthDevice(0, { root: null }),
      currentRoot,
    ], 0, 0, 10, { 0: staleRoot })).toMatchObject([{ device: 1 }]);
  });

  it('uses current-epoch root-specific authority for the root request baseline, never aggregate health cursors', () => {
    const device = healthDevice(0, { root: rootStatus({ role: 'root', cursor: 99 }) });
    const streamRoot = { device: 0, ...rootStatus({ role: 'leaf' }), cursor: 2 };
    expect(rootRequestBaseline(3, 0, device, 0, 1, 100, { 0: streamRoot })).toBe(3);
    expect(rootRequestBaseline(3, 0, device, 1, 1, 100, { 0: streamRoot })).toBe(99);
    expect(rootRequestBaseline(3, 0, undefined, null, 1, null, { 0: streamRoot })).toBe(3);
  });

  it('preserves an eligible explicit source, falls back to the lowest owner, and does not overlap queries', async () => {
    const fetchMock = acceptedGttFetch();
    stubFetch(fetchMock);
    const first = [healthDevice(0), healthDevice(1)];
    const { rerender } = render(topology(first));
    const ui = within(document.body);
    const selector = await ui.findByLabelText('Topology source');
    await waitFor(() => expect(fetchMock).toHaveBeenCalledTimes(1));
    expect(ui.getByRole('button', { name: 'GTT request pending' })).toHaveProperty('disabled', true);
    fireEvent.change(selector, { target: { value: '1' } });
    await waitFor(() => expect(fetchMock).toHaveBeenCalledTimes(2));
    expect((selector as HTMLSelectElement).value).toBe('1');

    rerender(topology([healthDevice(0), healthDevice(1)]));
    expect((ui.getByLabelText('Topology source') as HTMLSelectElement).value).toBe('1');
    rerender(topology([healthDevice(0), healthDevice(1, { connected: false })]));
    await waitFor(() => expect(ui.queryByLabelText('Topology source')).toBeNull());
    expect(ui.getByText(/Topology source: Device 0/)).not.toBeNull();
    await waitFor(() => expect(fetchMock).toHaveBeenCalledTimes(3));
  });

  it('waits for a strictly newer selected-source generation and never displays another source snapshot', async () => {
    const fetchMock = acceptedGttFetch();
    stubFetch(fetchMock);
    const initial = [
      healthDevice(0, { gtt: gttSnapshot({ generation: 1, entries: [gttEntry({ adva: '0102545678c0' })] }) }),
      healthDevice(1, { gtt: gttSnapshot({ generation: 1, entries: [gttEntry({ adva: '0102545678c1' })] }) }),
    ];
    const { rerender } = render(topology(initial));
    const ui = within(document.body);
    await waitFor(() => expect(fetchMock).toHaveBeenCalledTimes(1));
    await waitFor(() => expect(ui.getByText(/waiting for a newer GTT generation/i)).not.toBeNull());

    rerender(topology([
      initial[0],
      healthDevice(1, { gtt: gttSnapshot({ generation: 2, entries: [gttEntry({ adva: '0102545678c1' })] }) }),
    ]));
    expect(ui.queryByText('0102545678c1')).toBeNull();
    expect(ui.queryByText(/Members/)).toBeNull();

    fireEvent.change(ui.getByLabelText('Topology source'), { target: { value: '1' } });
    await waitFor(() => expect(fetchMock).toHaveBeenCalledTimes(2));
    rerender(topology([
      initial[0],
      healthDevice(1, {
        root: rootStatus({ node: 4, local: '0102545678c1', role: 'leaf' }),
        gtt: gttSnapshot({
          generation: 3,
          entries: [
            gttEntry({ adva: '0102545678c1', hop: 2, freshness: 'active', departed: 'false' }),
            gttEntry({ index: 1, adva: '0102545678c2', hop: 3, freshness: 'soft_stale', departed: 'unknown', serial_state: 'unknown' }),
          ],
        }),
      }),
    ]));
    await ui.findByText('0102545678c1');
    const knownNodeCard = ui.getByRole('heading', { name: 'Node 4' }).closest('article');
    expect(knownNodeCard).not.toBeNull();
    if (!knownNodeCard) throw new Error('Expected the matched node card.');
    expect(within(knownNodeCard).getByText('0102545678c1')).not.toBeNull();
    expect(ui.getByText(/Hop 2 · active/i)).not.toBeNull();
    expect(ui.getByText(/Hop 3 · soft stale/i)).not.toBeNull();
    expect(ui.getByText(/Unknown \(raw 4\)/)).not.toBeNull();
    expect(ui.queryByText(/parent|child/i)).toBeNull();
  });

  it('cancels old-epoch GTT work, restarts immediately, and accepts a lower new-host generation', async () => {
    vi.useFakeTimers();
    let resolveOldRequest: ((response: Response) => void) | undefined;
    let requestCount = 0;
    const fetchMock = vi.fn((input: string) => {
      if (input !== '/api/gtt') throw new Error(`Unexpected request ${input}`);
      requestCount += 1;
      if (requestCount === 1) {
        return new Promise<Response>((resolve) => { resolveOldRequest = resolve; });
      }
      return new Promise<Response>(() => undefined);
    });
    stubFetch(fetchMock);
    const oldSnapshot = gttSnapshot({ generation: 9, entries: [gttEntry({ adva: '0102545678c0' })] });
    const { rerender } = render(topology([healthDevice(0, { gtt: oldSnapshot })], { httpTimeoutMs: 100, confirmationTimeoutMs: 5 }));
    const ui = within(document.body);
    await act(async () => { await vi.advanceTimersByTimeAsync(0); });
    expect(fetchMock).toHaveBeenCalledTimes(1);

    rerender(topology([healthDevice(0, { gtt: oldSnapshot })], {
      currentEpoch: 1,
      healthEpoch: 0,
      httpTimeoutMs: 100,
      confirmationTimeoutMs: 100,
    }));
    await act(async () => { await vi.advanceTimersByTimeAsync(0); });
    expect(fetchMock).toHaveBeenCalledTimes(2);
    await act(async () => { await vi.advanceTimersByTimeAsync(5); });
    expect(ui.queryByText(/No newer GTT generation arrived in time/i)).toBeNull();
    expect(ui.queryByText('0102545678c0')).toBeNull();
    const resolve = resolveOldRequest;
    if (!resolve) throw new Error('Expected an old-epoch GTT request.');
    await act(async () => {
      resolve(jsonResponse(202, { schema: 'mind.command.v1', accepted: true, device: 0, command: 'gtt' }));
    });

    rerender(topology([
      healthDevice(0, { gtt: gttSnapshot({ generation: 1, entries: [gttEntry({ adva: '0102545678c1' })] }) }),
    ], { currentEpoch: 1, healthEpoch: 1, httpTimeoutMs: 100, confirmationTimeoutMs: 100 }));
    await act(async () => { await vi.advanceTimersByTimeAsync(0); });
    expect(ui.getByText('0102545678c1')).not.toBeNull();
    expect(ui.getByText('New GTT generation received.')).not.toBeNull();
    expect(fetchMock).toHaveBeenCalledTimes(2);
  });

  it('reports a bounded generation timeout and lets the operator retry', async () => {
    vi.useFakeTimers();
    const fetchMock = acceptedGttFetch();
    stubFetch(fetchMock);
    render(topology([healthDevice(0, { gtt: gttSnapshot({ generation: 1 }) })], {
      httpTimeoutMs: 100,
      confirmationTimeoutMs: 5,
    }));
    const ui = within(document.body);
    await act(async () => { await vi.advanceTimersByTimeAsync(0); });
    await act(async () => { await vi.advanceTimersByTimeAsync(5); });
    expect(ui.getByText(/No newer GTT generation arrived in time/i)).not.toBeNull();
    fireEvent.click(ui.getByRole('button', { name: 'Retry GTT' }));
    await act(async () => { await vi.advanceTimersByTimeAsync(0); });
    expect(fetchMock).toHaveBeenCalledTimes(2);
  });

  it('keeps the last complete selected-source GTT visible while refreshing', async () => {
    vi.useFakeTimers();
    const fetchMock = acceptedGttFetch();
    stubFetch(fetchMock);
    const generation1 = gttSnapshot({ generation: 1, entries: [gttEntry({ adva: '0102545678c0' })] });
    const { rerender } = render(topology([healthDevice(0, { gtt: generation1 })], { refreshIntervalMs: 5 }));
    const ui = within(document.body);
    await act(async () => { await vi.advanceTimersByTimeAsync(0); });

    const generation2 = gttSnapshot({ generation: 2, entries: [gttEntry({ adva: '0102545678c1' })] });
    rerender(topology([healthDevice(0, { gtt: generation2 })], { refreshIntervalMs: 5 }));
    await act(async () => { await vi.advanceTimersByTimeAsync(0); });
    expect(ui.getByText('0102545678c1')).not.toBeNull();

    await act(async () => { await vi.advanceTimersByTimeAsync(5); });
    expect(fetchMock).toHaveBeenCalledTimes(2);
    expect(ui.getByText('0102545678c1')).not.toBeNull();
    expect(ui.getByRole('button', { name: 'GTT request pending' })).toHaveProperty('disabled', true);
  });

  it('groups cards by hop and freshness and computes staleness from host completion time only', () => {
    const groups = groupGttEntries([
      gttEntry({ index: 0, hop: 2, freshness: 'active', departed: 'false' }),
      gttEntry({ index: 1, adva: '0102545678c1', hop: 2, freshness: 'active', departed: 'unknown' }),
      gttEntry({ index: 2, adva: '0102545678c2', hop: 3, freshness: 'departed', departed: 'true' }),
    ]);
    expect(groups).toHaveLength(2);
    expect(groups[0]).toMatchObject({ hopLabel: 'Hop 2', freshnessLabel: 'active', entries: [{ index: 0 }, { index: 1 }] });
    expect(isGttStale(gttSnapshot({ completed_at_ms: 5_000, query_at_ms: 0 }), 20_000)).toBe(true);
    expect(isGttStale(gttSnapshot({ completed_at_ms: 5_001, query_at_ms: 0 }), 20_000)).toBe(false);
  });
});
