import '../test/runtime';
import { StrictMode, type ComponentProps } from 'react';
import { act, cleanup, fireEvent, render, waitFor, within } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { FloorplanPanel } from './FloorplanPanel';
import { eventRecord, gttEntry, gttSnapshot, healthDevice, jsonResponse, layoutReady, rootStatus } from '../test/fixtures';
import { restoreFetch, stubFetch } from '../test/runtime';
import type { IncidentLocalizationView } from '../localization/IncidentLocalization';
import type { LogicalEvent } from '../state/store';

afterEach(() => {
  cleanup();
  restoreFetch();
  vi.unstubAllGlobals();
  vi.useRealTimers();
});

function roster() {
  const local = '1842de524add';
  return [healthDevice(0, {
    root: rootStatus({ local }),
    gtt: gttSnapshot({ local, entries: [gttEntry({ index: 0, adva: local }), gttEntry({ index: 1, adva: '0102545678c1' }), gttEntry({ index: 2, adva: '0102545678c2' })] }),
  })];
}

function panel(overrides: Partial<ComponentProps<typeof FloorplanPanel>> = {}) {
  return <FloorplanPanel devices={roster()} healthEpoch={0} currentEpoch={0} healthCurrentCursor={1} rootRecords={{}} now={12_000} {...overrides} />;
}

describe('floorplan interactions', () => {
  it('keeps unpositioned GTT entries staged until an intentional center placement persists a full sorted replacement', async () => {
    const initial = layoutReady({ positions: [{ adva: '0102545678c2', x: 0.9, y: 0.1 }] });
    const saved = layoutReady({ revision: 1, positions: [
      { adva: '0102545678c1', x: 0.5, y: 0.5 },
      { adva: '0102545678c2', x: 0.9, y: 0.1 },
    ] });
    const fetchMock = vi.fn((input: string, init?: RequestInit) => {
      if (input === '/api/layout' && !init?.method) return Promise.resolve(jsonResponse(200, initial));
      if (input === '/api/layout' && init?.method === 'PUT') return Promise.resolve(jsonResponse(200, saved));
      if (input === '/api/gtt') return Promise.resolve(jsonResponse(202, { schema: 'mind.command.v1', accepted: true, device: 0, command: 'gtt' }));
      throw new Error(`Unexpected ${input}`);
    });
    stubFetch(fetchMock);
    render(panel());
    const ui = within(document.body);
    await ui.findByText('Unpositioned nodes');
    expect(ui.getAllByText('Unpositioned').length).toBeGreaterThanOrEqual(2);
    const staging = ui.getByRole('region', { name: /Unpositioned nodes/i });
    const remote = within(staging).getByText('0102545678c1').closest('li');
    if (!remote) throw new Error('Expected the remote staging entry.');
    fireEvent.click(within(remote).getByRole('button', { name: 'Place at center' }));
    await waitFor(() => expect(fetchMock).toHaveBeenCalledWith('/api/layout', expect.objectContaining({
      method: 'PUT',
      body: '{"schema":"mind.dashboard.layout.update.v1","base_revision":0,"positions":[{"adva":"0102545678c1","x":0.5,"y":0.5},{"adva":"0102545678c2","x":0.9,"y":0.1}]}',
    })));
    await ui.findByText('Node placed at center.');
  });

  it('renders blank and intrinsic-image frames and retains positions through an image response', async () => {
    const imageLayout = layoutReady({
      revision: 2,
      floorplan: { sha256: 'a'.repeat(64), mime: 'image/png', width: 1200, height: 800, url: `/api/floorplan/${'a'.repeat(64)}` },
      positions: [{ adva: '1842de524add', x: 0.2, y: 0.3 }],
    });
    stubFetch(vi.fn((input: string, init?: RequestInit) => {
      if (input === '/api/layout' && !init?.method) return Promise.resolve(jsonResponse(200, imageLayout));
      if (input === '/api/gtt') return Promise.resolve(jsonResponse(202, { schema: 'mind.command.v1', accepted: true, device: 0, command: 'gtt' }));
      throw new Error(`Unexpected ${input}`);
    }));
    render(panel());
    const image = await within(document.body).findByAltText('Uploaded floorplan');
    expect(image.getAttribute('src')).toBe(imageLayout.floorplan?.url);
    expect(within(document.body).getByRole('button', { name: /Gateway, positioned at x 0.20, y 0.30/i })).not.toBeNull();
  });

  it('applies numeric blank and uploaded floorplan aspects to the confirmed map content', async () => {
    const blankFetch = vi.fn((input: string) => {
      if (input === '/api/layout') return Promise.resolve(jsonResponse(200, layoutReady()));
      throw new Error(`Unexpected ${input}`);
    });
    stubFetch(blankFetch);
    const { unmount } = render(panel({ healthAuthoritative: false }));
    await within(document.body).findByText('Floorplan coordinates loaded. No image uploaded.');
    expect((document.querySelector('.floorplan-content') as HTMLElement).style.aspectRatio).toBe(`${16 / 9} / 1`);
    unmount();

    const image = { sha256: 'a'.repeat(64), mime: 'image/png' as const, width: 1200, height: 800, url: `/api/floorplan/${'a'.repeat(64)}` };
    stubFetch(vi.fn((input: string) => {
      if (input === '/api/layout') return Promise.resolve(jsonResponse(200, layoutReady({ floorplan: image })));
      throw new Error(`Unexpected ${input}`);
    }));
    render(panel({ healthAuthoritative: false }));
    await within(document.body).findByAltText('Uploaded floorplan');
    expect((document.querySelector('.floorplan-content') as HTMLElement).style.aspectRatio).toBe('1.5 / 1');
  });

  it('bootstraps only from current authoritative health, bounds the request, and permits retry', async () => {
    vi.useFakeTimers();
    const missing = [healthDevice(0, { root: rootStatus(), gtt: null })];
    const fetchMock = vi.fn((input: string, init?: RequestInit) => {
      if (input === '/api/layout') return Promise.resolve(jsonResponse(200, layoutReady()));
      if (input === '/api/gtt') {
        return new Promise<Response>((_resolve, reject) => {
          init?.signal?.addEventListener('abort', () => reject(new DOMException('Aborted', 'AbortError')));
        });
      }
      throw new Error(`Unexpected ${input}`);
    });
    stubFetch(fetchMock);
    const { rerender } = render(panel({ devices: missing, healthAuthoritative: false, gttRequestTimeoutMs: 5, gttRefreshIntervalMs: 60_000 }));
    await act(async () => { await vi.advanceTimersByTimeAsync(0); });
    const ui = within(document.body);
    expect(fetchMock.mock.calls.filter(([input]) => input === '/api/gtt')).toHaveLength(0);
    rerender(panel({ devices: missing, gttRequestTimeoutMs: 5, gttRefreshIntervalMs: 60_000 }));
    await act(async () => { await vi.advanceTimersByTimeAsync(0); });
    expect(fetchMock).toHaveBeenCalledWith('/api/gtt', expect.objectContaining({ body: '{"device":0}' }));
    for (let count = 0; count < 3; count += 1) {
      rerender(panel({ devices: [{ ...missing[0] }], gttRequestTimeoutMs: 5, gttRefreshIntervalMs: 60_000 }));
      await act(async () => { await vi.advanceTimersByTimeAsync(1); });
    }
    expect(fetchMock.mock.calls.filter(([input]) => input === '/api/gtt')).toHaveLength(1);
    await act(async () => { await vi.advanceTimersByTimeAsync(5); });
    expect(ui.getByText('Gateway GTT request timed out. Retry Gateway.')).not.toBeNull();
    fireEvent.click(ui.getByRole('button', { name: 'Request Gateway GTT' }));
    expect(fetchMock.mock.calls.filter(([input]) => input === '/api/gtt')).toHaveLength(2);
  });

  it('confirms only a strictly greater same-epoch GTT generation', async () => {
    const oldGeneration = roster();
    oldGeneration[0] = { ...oldGeneration[0], gtt: gttSnapshot({
      generation: 9,
      local: '1842de524add',
      entries: [gttEntry({ adva: '1842de524add' })],
    }) };
    const fetchMock = vi.fn((input: string, _init?: RequestInit) => {
      if (input === '/api/layout') return Promise.resolve(jsonResponse(200, layoutReady()));
      if (input === '/api/gtt') return Promise.resolve(jsonResponse(202, { schema: 'mind.command.v1', accepted: true, device: 0, command: 'gtt' }));
      throw new Error(`Unexpected ${input}`);
    });
    stubFetch(fetchMock);
    const { rerender } = render(panel({ devices: oldGeneration, gttRefreshIntervalMs: 60_000 }));
    const ui = within(document.body);
    await ui.findByRole('button', { name: 'Refresh GTT' });
    await ui.findByText('Floorplan coordinates loaded. No image uploaded.');
    fireEvent.click(ui.getByRole('button', { name: 'Refresh GTT' }));
    await waitFor(() => expect(fetchMock).toHaveBeenCalledWith('/api/gtt', expect.anything()));
    rerender(panel({ devices: oldGeneration.map((device) => ({ ...device })), gttRefreshIntervalMs: 60_000 }));
    await ui.findByText('Waiting for Gateway GTT update.');
    expect(ui.getByRole('button', { name: 'Refreshing GTT' })).toHaveProperty('disabled', true);
    const restarted = [healthDevice(0, { root: rootStatus(), gtt: gttSnapshot({
      generation: 1,
      local: '1842de524add',
      entries: [gttEntry({ adva: '1842de524add' })],
    }) })];
    rerender(panel({ devices: restarted, gttRefreshIntervalMs: 60_000 }));
    expect(ui.getByText('Waiting for Gateway GTT update.')).not.toBeNull();
    expect(ui.getByRole('button', { name: 'Refreshing GTT' })).toHaveProperty('disabled', true);
    rerender(panel({ devices: [healthDevice(0, { root: rootStatus(), gtt: gttSnapshot({
      generation: 10,
      local: '1842de524add',
      entries: [gttEntry({ adva: '1842de524add' })],
    }) })], gttRefreshIntervalMs: 60_000 }));
    await ui.findByText('Gateway GTT updated.');
  });

  it('confirms an authoritative GTT advance that arrives before the POST resolves', async () => {
    let resolvePost: ((response: Response) => void) | undefined;
    const missing = [healthDevice(0, { root: rootStatus(), gtt: null })];
    stubFetch(vi.fn((input: string) => {
      if (input === '/api/layout') return Promise.resolve(jsonResponse(200, layoutReady()));
      if (input === '/api/gtt') return new Promise<Response>((resolve) => { resolvePost = resolve; });
      throw new Error(`Unexpected ${input}`);
    }));
    const { rerender } = render(panel({ devices: missing, gttRefreshIntervalMs: 60_000 }));
    const ui = within(document.body);
    await ui.findByText('Refreshing Gateway GTT.');
    rerender(panel({ devices: [healthDevice(0, { root: rootStatus(), gtt: gttSnapshot({
      generation: 2,
      local: '1842de524add',
      entries: [gttEntry({ adva: '1842de524add' })],
    }) })], gttRefreshIntervalMs: 60_000 }));
    await act(async () => { resolvePost?.(jsonResponse(202, { schema: 'mind.command.v1', accepted: true, device: 0, command: 'gtt' })); });
    await ui.findByText('Gateway GTT updated.');
    expect(ui.getByRole('button', { name: 'Refresh GTT' })).toHaveProperty('disabled', false);
  });

  it('cancels an old epoch request and permits a lower generation only after new-epoch health becomes authoritative', async () => {
    const fetchMock = vi.fn((input: string) => {
      if (input === '/api/layout') return Promise.resolve(jsonResponse(200, layoutReady()));
      if (input === '/api/gtt') return Promise.resolve(jsonResponse(202, { schema: 'mind.command.v1', accepted: true, device: 0, command: 'gtt' }));
      throw new Error(`Unexpected ${input}`);
    });
    stubFetch(fetchMock);
    const beforeReset = [healthDevice(0, { root: rootStatus(), gtt: gttSnapshot({
      generation: 9,
      local: '1842de524add',
      entries: [gttEntry({ adva: '1842de524add' })],
    }) })];
    const { rerender } = render(panel({ devices: beforeReset, gttRefreshIntervalMs: 60_000 }));
    const ui = within(document.body);
    await ui.findByText('Floorplan coordinates loaded. No image uploaded.');
    fireEvent.click(ui.getByRole('button', { name: 'Refresh GTT' }));
    await waitFor(() => expect(fetchMock.mock.calls.filter(([input]) => input === '/api/gtt')).toHaveLength(1));
    rerender(panel({ devices: beforeReset, currentEpoch: 1, healthEpoch: 0, gttRefreshIntervalMs: 60_000 }));
    await ui.findByText('Gateway GTT refresh was reset with the bridge. Await current Gateway health before retrying.');
    expect(ui.queryByRole('button', { name: 'Request Gateway GTT' })).toBeNull();
    const afterReset = [healthDevice(0, { root: rootStatus(), gtt: gttSnapshot({
      generation: 1,
      local: '1842de524add',
      entries: [gttEntry({ adva: '1842de524add' })],
    }) })];
    rerender(panel({ devices: afterReset, currentEpoch: 1, healthEpoch: 1, gttRefreshIntervalMs: 60_000 }));
    fireEvent.click(ui.getByRole('button', { name: 'Refresh GTT' }));
    await waitFor(() => expect(fetchMock.mock.calls.filter(([input]) => input === '/api/gtt')).toHaveLength(2));
    rerender(panel({ devices: [healthDevice(0, { root: rootStatus(), gtt: gttSnapshot({
      generation: 2,
      local: '1842de524add',
      entries: [gttEntry({ adva: '1842de524add' })],
    }) })], currentEpoch: 1, healthEpoch: 1, gttRefreshIntervalMs: 60_000 }));
    await ui.findByText('Gateway GTT updated.');
  });

  it('keeps periodic GTT refresh stable across repeated decoded-device identities', async () => {
    vi.useFakeTimers();
    const fetchMock = vi.fn((input: string) => {
      if (input === '/api/layout') return Promise.resolve(jsonResponse(200, layoutReady()));
      if (input === '/api/gtt') return Promise.resolve(jsonResponse(202, { schema: 'mind.command.v1', accepted: true, device: 0, command: 'gtt' }));
      throw new Error(`Unexpected ${input}`);
    });
    stubFetch(fetchMock);
    const { rerender } = render(panel({ gttRefreshIntervalMs: 10 }));
    for (let count = 0; count < 5; count += 1) {
      await act(async () => { await vi.advanceTimersByTimeAsync(5); });
      rerender(panel({ devices: roster().map((device) => ({ ...device })), gttRefreshIntervalMs: 10 }));
    }
    expect(fetchMock.mock.calls.filter(([input]) => input === '/api/gtt')).toHaveLength(1);
  });

  it('clears a selected placement when retained health is no longer authoritative', async () => {
    stubFetch(vi.fn((input: string) => {
      if (input === '/api/layout') return Promise.resolve(jsonResponse(200, layoutReady()));
      throw new Error(`Unexpected ${input}`);
    }));
    const { rerender } = render(panel());
    const ui = within(document.body);
    const staging = await ui.findByRole('region', { name: /Unpositioned nodes/i });
    await ui.findByText('Floorplan coordinates loaded. No image uploaded.');
    fireEvent.click(within(staging).getAllByRole('button', { name: 'Place on map' })[0]!);
    expect(document.querySelector('.placement-active')).not.toBeNull();
    rerender(panel({ healthAuthoritative: false }));
    await ui.findByText('Placement canceled because the node left the authoritative Gateway roster.');
    expect(document.querySelector('.placement-active')).toBeNull();
    expect(ui.getByText(/Gateway topology unavailable while Gateway health is refreshing/i)).not.toBeNull();
  });

  it('persists pointer and keyboard movement with clamped normalized coordinates', async () => {
    const initial = layoutReady({ positions: [{ adva: '1842de524add', x: 0.2, y: 0.3 }] });
    const keyboardSaved = layoutReady({ revision: 1, positions: [{ adva: '1842de524add', x: 0.21, y: 0.3 }] });
    const pointerSaved = layoutReady({ revision: 2, positions: [{ adva: '1842de524add', x: 0.6, y: 0.7 }] });
    const fetchMock = vi.fn()
      .mockResolvedValueOnce(jsonResponse(200, initial))
      .mockResolvedValueOnce(jsonResponse(200, keyboardSaved))
      .mockResolvedValueOnce(jsonResponse(200, pointerSaved));
    stubFetch(fetchMock);
    render(panel());
    const ui = within(document.body);
    const node = await ui.findByRole('button', { name: /Gateway, positioned at x 0.20, y 0.30/i });
    fireEvent.keyDown(node, { key: 'ArrowRight' });
    fireEvent.keyUp(node, { key: 'ArrowRight' });
    await waitFor(() => expect(fetchMock.mock.calls.filter(([input]) => input === '/api/layout')).toHaveLength(2));
    const keyboardBody = JSON.parse(String(fetchMock.mock.calls[1]?.[1] && (fetchMock.mock.calls[1][1] as RequestInit).body)) as { positions: Array<{ x: number; y: number }> };
    expect(keyboardBody.positions).toEqual([{ adva: '1842de524add', x: expect.closeTo(0.21), y: 0.3 }]);
    const content = document.querySelector('.floorplan-content');
    if (!(content instanceof HTMLElement)) throw new Error('Expected floorplan content.');
    Object.defineProperty(content, 'getBoundingClientRect', { configurable: true, value: () => ({ left: 0, top: 0, width: 100, height: 100 }) });
    fireEvent.pointerDown(node, { pointerId: 1, clientX: 21, clientY: 30 });
    fireEvent.pointerMove(node, { pointerId: 1, clientX: 60, clientY: 70 });
    fireEvent.pointerUp(node, { pointerId: 1, clientX: 60, clientY: 70 });
    await waitFor(() => expect(fetchMock.mock.calls.filter(([input]) => input === '/api/layout')).toHaveLength(3));
    const pointerBody = JSON.parse(String(fetchMock.mock.calls[2]?.[1] && (fetchMock.mock.calls[2][1] as RequestInit).body)) as { positions: Array<{ x: number; y: number }> };
    expect(pointerBody.positions).toEqual([{ adva: '1842de524add', x: 0.6, y: 0.7 }]);
  });

  it('reapplies a position mutation once after a CAS conflict', async () => {
    const initial = layoutReady();
    const concurrent = layoutReady({ revision: 1, positions: [{ adva: '1842de524add', x: 0.2, y: 0.3 }] });
    const saved = layoutReady({ revision: 2, positions: [
      { adva: '0102545678c1', x: 0.5, y: 0.5 },
      { adva: '1842de524add', x: 0.2, y: 0.3 },
    ] });
    const fetchMock = vi.fn()
      .mockResolvedValueOnce(jsonResponse(200, initial))
      .mockResolvedValueOnce(jsonResponse(409, { schema: 'mind.dashboard.layout.conflict.v1', error: 'revision_conflict', current: concurrent }))
      .mockResolvedValueOnce(jsonResponse(200, saved));
    stubFetch(fetchMock);
    render(panel());
    const ui = within(document.body);
    const staging = await ui.findByRole('region', { name: /Unpositioned nodes/i });
    const placeAtCenter = within(staging).getAllByRole('button', { name: 'Place at center' })[0]!;
    await waitFor(() => expect(placeAtCenter).toHaveProperty('disabled', false));
    fireEvent.click(placeAtCenter);
    await ui.findByText('Node placed at center. Reapplied after a concurrent update.');
    expect(fetchMock.mock.calls.filter(([input]) => input === '/api/layout')).toHaveLength(3);
  });

  it('does not retry a roster-bound conflict after the affected GTT entry leaves the current roster', async () => {
    let resolveConflict: ((response: Response) => void) | undefined;
    let layoutReads = 0;
    const initial = layoutReady();
    const concurrent = layoutReady({ revision: 1, positions: [{ adva: '1842de524add', x: 0.2, y: 0.3 }] });
    const onLayoutChange = vi.fn();
    const fetchMock = vi.fn((input: string, init?: RequestInit) => {
      if (input === '/api/layout' && !init?.method) {
        return Promise.resolve(jsonResponse(200, layoutReads++ === 0 ? initial : concurrent));
      }
      if (input === '/api/layout' && init?.method === 'PUT') {
        return new Promise<Response>((resolve) => { resolveConflict = resolve; });
      }
      throw new Error(`Unexpected ${input}`);
    });
    stubFetch(fetchMock);
    const { rerender } = render(panel({ onLayoutChange }));
    const ui = within(document.body);
    const staging = await ui.findByRole('region', { name: /Unpositioned nodes/i });
    onLayoutChange.mockClear();
    const remote = within(staging).getByText('0102545678c1').closest('li');
    if (!remote) throw new Error('Expected current remote node.');
    fireEvent.click(within(remote).getByRole('button', { name: 'Place at center' }));
    await waitFor(() => expect(resolveConflict).toBeDefined());

    const local = '1842de524add';
    rerender(panel({
      devices: [healthDevice(0, {
        root: rootStatus({ local }),
        gtt: gttSnapshot({ local, entries: [gttEntry({ adva: local })] }),
      })],
      onLayoutChange,
    }));
    await act(async () => { resolveConflict?.(jsonResponse(409, { schema: 'mind.dashboard.layout.conflict.v1', error: 'revision_conflict', current: concurrent })); });

    await waitFor(() => expect(onLayoutChange).toHaveBeenLastCalledWith(concurrent));
    expect(fetchMock.mock.calls.filter(([path, options]) => path === '/api/layout' && (options as RequestInit).method === 'PUT')).toHaveLength(1);
    expect(fetchMock.mock.calls.filter(([path, options]) => path === '/api/layout' && !(options as RequestInit).method)).toHaveLength(2);
    expect(onLayoutChange).not.toHaveBeenCalledWith(expect.objectContaining({ positions: expect.arrayContaining([
      expect.objectContaining({ adva: '0102545678c1', x: 0.5, y: 0.5 }),
    ]) }));
  });

  it('does not retry an off-roster unplace after GTT authority becomes invalid', async () => {
    let resolveConflict: ((response: Response) => void) | undefined;
    let layoutReads = 0;
    const orphan = { adva: 'ffffffffffff', x: 0.1, y: 0.2 };
    const initial = layoutReady({ positions: [orphan] });
    const concurrent = layoutReady({ revision: 1, positions: [{ adva: '1842de524add', x: 0.2, y: 0.3 }] });
    const onLayoutChange = vi.fn();
    const fetchMock = vi.fn((input: string, init?: RequestInit) => {
      if (input === '/api/layout' && !init?.method) {
        return Promise.resolve(jsonResponse(200, layoutReads++ === 0 ? initial : concurrent));
      }
      if (input === '/api/layout' && init?.method === 'PUT') {
        return new Promise<Response>((resolve) => { resolveConflict = resolve; });
      }
      throw new Error(`Unexpected ${input}`);
    });
    stubFetch(fetchMock);
    const { rerender } = render(panel({ onLayoutChange }));
    const ui = within(document.body);
    const orphanRow = (await ui.findByText('ffffffffffff')).closest('tr');
    if (!orphanRow) throw new Error('Expected stored orphan row.');
    onLayoutChange.mockClear();
    fireEvent.click(within(orphanRow).getByRole('button', { name: 'Unplace' }));
    await waitFor(() => expect(resolveConflict).toBeDefined());

    const local = '1842de524add';
    rerender(panel({
      devices: [healthDevice(0, {
        root: rootStatus({ local }),
        gtt: gttSnapshot({ local, entries: [gttEntry({ adva: '0102545678ff' })] }),
      })],
      onLayoutChange,
    }));
    await act(async () => { resolveConflict?.(jsonResponse(409, { schema: 'mind.dashboard.layout.conflict.v1', error: 'revision_conflict', current: concurrent })); });

    await waitFor(() => expect(onLayoutChange).toHaveBeenLastCalledWith(concurrent));
    expect(fetchMock.mock.calls.filter(([path, options]) => path === '/api/layout' && (options as RequestInit).method === 'PUT')).toHaveLength(1);
    expect(fetchMock.mock.calls.filter(([path, options]) => path === '/api/layout' && !(options as RequestInit).method)).toHaveLength(2);
    expect(onLayoutChange).not.toHaveBeenCalledWith(expect.objectContaining({ positions: [] }));
  });

  it('disables layout editing while a failed mutation recovers and does not retain its optimistic result', async () => {
    let recover: ((response: Response) => void) | undefined;
    let layoutReads = 0;
    const initial = layoutReady({ positions: [{ adva: '1842de524add', x: 0.2, y: 0.3 }] });
    const fetchMock = vi.fn((input: string, init?: RequestInit) => {
      if (input === '/api/layout' && !init?.method && layoutReads++ === 0) return Promise.resolve(jsonResponse(200, initial));
      if (input === '/api/layout' && init?.method === 'PUT') return Promise.resolve(jsonResponse(503, { schema: 'mind.dashboard.error.v1', accepted: false, error: 'storage_unavailable' }));
      if (input === '/api/layout' && !init?.method) return new Promise<Response>((resolve) => { recover = resolve; });
      throw new Error(`Unexpected ${input}`);
    });
    stubFetch(fetchMock);
    render(panel());
    const ui = within(document.body);
    const unplace = await ui.findByRole('button', { name: 'Unplace' });
    fireEvent.click(unplace);
    await waitFor(() => expect(ui.getByText(/Floorplan save failed: Layout request failed: storage_unavailable/i)).not.toBeNull());
    expect(ui.getAllByRole('button', { name: 'Place at center' })[0]).toHaveProperty('disabled', true);
    if (!recover) throw new Error('Expected layout recovery request.');
    await act(async () => { recover?.(jsonResponse(200, initial)); });
    await ui.findByText('Floorplan recovered after save failure.');
    expect(ui.getByRole('button', { name: /Gateway, positioned at x 0.20, y 0.30/i })).not.toBeNull();
  });

  it('rolls back immediately and remains non-editable when the recovery GET fails', async () => {
    let layoutReads = 0;
    const initial = layoutReady({ positions: [{ adva: '1842de524add', x: 0.2, y: 0.3 }] });
    stubFetch(vi.fn((input: string, init?: RequestInit) => {
      if (input === '/api/layout' && !init?.method) {
        const read = layoutReads++;
        return Promise.resolve(read === 1 ? jsonResponse(503, { schema: 'mind.dashboard.error.v1', accepted: false, error: 'storage_unavailable' }) : jsonResponse(200, initial));
      }
      if (input === '/api/layout' && init?.method === 'PUT') return Promise.resolve(jsonResponse(503, { schema: 'mind.dashboard.error.v1', accepted: false, error: 'storage_unavailable' }));
      throw new Error(`Unexpected ${input}`);
    }));
    render(panel());
    const ui = within(document.body);
    fireEvent.click(await ui.findByRole('button', { name: 'Unplace' }));
    await ui.findByText(/Floorplan unavailable: Layout request failed: storage_unavailable/i);
    const node = ui.getByRole('button', { name: /Gateway, positioned at x 0.20, y 0.30/i });
    expect(node).toHaveProperty('disabled', true);
    expect(ui.getByRole('button', { name: 'Unplace' })).toHaveProperty('disabled', true);
    fireEvent.click(ui.getByRole('button', { name: 'Retry floorplan' }));
    await ui.findByText('Floorplan recovered.');
    expect(ui.getByRole('button', { name: 'Unplace' })).toHaveProperty('disabled', false);
  });

  it('bounds initial and retried layout reads without retaining a timed-out read', async () => {
    vi.useFakeTimers();
    let aborts = 0;
    stubFetch(vi.fn((input: string, init?: RequestInit) => {
      if (input !== '/api/layout') throw new Error(`Unexpected ${input}`);
      return new Promise<Response>((_resolve, reject) => {
        init?.signal?.addEventListener('abort', () => {
          aborts += 1;
          reject(new DOMException('Aborted', 'AbortError'));
        });
      });
    }));
    const { unmount } = render(panel({ healthAuthoritative: false, layoutReadTimeoutMs: 5 }));
    const ui = within(document.body);
    await act(async () => { await vi.advanceTimersByTimeAsync(5); });
    expect(ui.getByText('Floorplan request timed out. Retry floorplan.')).not.toBeNull();
    expect(aborts).toBe(1);
    fireEvent.click(ui.getByRole('button', { name: 'Retry floorplan' }));
    await act(async () => { await vi.advanceTimersByTimeAsync(5); });
    expect(ui.getByText('Floorplan request timed out. Retry floorplan.')).not.toBeNull();
    expect(aborts).toBe(2);
    expect(vi.getTimerCount()).toBe(0);
    unmount();
    expect(vi.getTimerCount()).toBe(0);
  });

  it('bounds a hung recovery layout read while retaining the last safe layout', async () => {
    vi.useFakeTimers();
    let reads = 0;
    let recoveryAborted = false;
    const initial = layoutReady({ positions: [{ adva: '1842de524add', x: 0.2, y: 0.3 }] });
    stubFetch(vi.fn((input: string, init?: RequestInit) => {
      if (input === '/api/layout' && !init?.method && reads++ === 0) return Promise.resolve(jsonResponse(200, initial));
      if (input === '/api/layout' && !init?.method) {
        return new Promise<Response>((_resolve, reject) => {
          init?.signal?.addEventListener('abort', () => {
            recoveryAborted = true;
            reject(new DOMException('Aborted', 'AbortError'));
          });
        });
      }
      if (input === '/api/layout' && init?.method === 'PUT') return Promise.resolve(jsonResponse(503, { schema: 'mind.dashboard.error.v1', accepted: false, error: 'storage_unavailable' }));
      throw new Error(`Unexpected ${input}`);
    }));
    const { unmount } = render(panel({ layoutReadTimeoutMs: 5 }));
    const ui = within(document.body);
    await act(async () => { await vi.advanceTimersByTimeAsync(0); });
    fireEvent.click(ui.getByRole('button', { name: 'Unplace' }));
    await act(async () => { await vi.advanceTimersByTimeAsync(5); });
    expect(recoveryAborted).toBe(true);
    expect(ui.getByText('Floorplan request timed out. Retry floorplan.')).not.toBeNull();
    expect(ui.getByRole('button', { name: /Gateway, positioned at x 0.20, y 0.30/i })).toHaveProperty('disabled', true);
    expect(ui.getByRole('button', { name: 'Retry floorplan' })).not.toBeNull();
    unmount();
    expect(vi.getTimerCount()).toBe(0);
  });

  it('rolls back to the newest conflict snapshot when the CAS retry also conflicts', async () => {
    let recover: ((response: Response) => void) | undefined;
    let layoutReads = 0;
    const initial = layoutReady();
    const firstCurrent = layoutReady({ revision: 1, positions: [{ adva: '1842de524add', x: 0.2, y: 0.3 }] });
    const secondCurrent = layoutReady({ revision: 2, positions: [{ adva: '1842de524add', x: 0.7, y: 0.6 }] });
    const fetchMock = vi.fn((input: string, init?: RequestInit) => {
      if (input === '/api/layout' && !init?.method && layoutReads++ === 0) return Promise.resolve(jsonResponse(200, initial));
      if (input === '/api/layout' && init?.method === 'PUT') {
        const puts = fetchMock.mock.calls.filter(([path]) => path === '/api/layout').filter(([, options]) => (options as RequestInit).method === 'PUT').length;
        const current = puts === 1 ? firstCurrent : secondCurrent;
        return Promise.resolve(jsonResponse(409, { schema: 'mind.dashboard.layout.conflict.v1', error: 'revision_conflict', current }));
      }
      if (input === '/api/layout') return new Promise<Response>((resolve) => { recover = resolve; });
      throw new Error(`Unexpected ${input}`);
    });
    stubFetch(fetchMock);
    render(panel());
    const ui = within(document.body);
    const staging = await ui.findByRole('region', { name: /Unpositioned nodes/i });
    const placeAtCenter = within(staging).getAllByRole('button', { name: 'Place at center' })[0]!;
    await waitFor(() => expect(placeAtCenter).toHaveProperty('disabled', false));
    fireEvent.click(placeAtCenter);
    await waitFor(() => expect(fetchMock.mock.calls.filter(([path]) => path === '/api/layout')).toHaveLength(4));
    const rolledBack = ui.getByRole('button', { name: /Gateway, positioned at x 0.70, y 0.60/i });
    expect(rolledBack).toHaveProperty('disabled', true);
    expect(within(staging).getAllByRole('button', { name: 'Place at center' })[0]).toHaveProperty('disabled', true);
    if (!recover) throw new Error('Expected recovery GET.');
    await act(async () => { recover?.(jsonResponse(200, secondCurrent)); });
    await ui.findByText('Floorplan recovered after save failure.');
  });

  it('times out and aborts a position mutation before recovering its baseline', async () => {
    vi.useFakeTimers();
    let layoutReads = 0;
    let aborted = false;
    const initial = layoutReady({ positions: [{ adva: '1842de524add', x: 0.2, y: 0.3 }] });
    stubFetch(vi.fn((input: string, init?: RequestInit) => {
      if (input === '/api/layout' && !init?.method) {
        layoutReads += 1;
        return Promise.resolve(jsonResponse(200, initial));
      }
      if (input === '/api/layout' && init?.method === 'PUT') {
        return new Promise<Response>((_resolve, reject) => {
          init.signal?.addEventListener('abort', () => {
            aborted = true;
            reject(new DOMException('Aborted', 'AbortError'));
          });
        });
      }
      throw new Error(`Unexpected ${input}`);
    }));
    render(panel({ layoutMutationTimeoutMs: 5 }));
    const ui = within(document.body);
    await act(async () => { await vi.advanceTimersByTimeAsync(0); });
    fireEvent.click(ui.getByRole('button', { name: 'Unplace' }));
    await act(async () => {
      await vi.advanceTimersByTimeAsync(5);
      await Promise.resolve();
    });
    expect(ui.getByText('Floorplan recovered after save timeout.')).not.toBeNull();
    expect(aborted).toBe(true);
    expect(layoutReads).toBe(2);
    expect(ui.getByRole('button', { name: /Gateway, positioned at x 0.20, y 0.30/i })).toHaveProperty('disabled', false);
  });

  it('aborts an owned layout mutation on unmount without allowing its completion to update the panel', async () => {
    let signal: AbortSignal | undefined;
    const initial = layoutReady({ positions: [{ adva: '1842de524add', x: 0.2, y: 0.3 }] });
    stubFetch(vi.fn((input: string, init?: RequestInit) => {
      if (input === '/api/layout' && !init?.method) return Promise.resolve(jsonResponse(200, initial));
      if (input === '/api/layout' && init?.method === 'PUT') {
        signal = init.signal ?? undefined;
        return new Promise<Response>(() => undefined);
      }
      throw new Error(`Unexpected ${input}`);
    }));
    const { unmount } = render(panel({ layoutMutationTimeoutMs: 60_000 }));
    const ui = within(document.body);
    fireEvent.click(await ui.findByRole('button', { name: 'Unplace' }));
    await waitFor(() => expect(signal).toBeDefined());
    unmount();
    expect(signal?.aborted).toBe(true);
  });

  it('applies the same timeout ownership to floorplan removal', async () => {
    vi.useFakeTimers();
    let removeAborted = false;
    const image = { sha256: 'a'.repeat(64), mime: 'image/png' as const, width: 800, height: 600, url: `/api/floorplan/${'a'.repeat(64)}` };
    const initial = layoutReady({ floorplan: image, positions: [{ adva: '1842de524add', x: 0.2, y: 0.3 }] });
    stubFetch(vi.fn((input: string, init?: RequestInit) => {
      if (input === '/api/layout') return Promise.resolve(jsonResponse(200, initial));
      if (input === '/api/floorplan/remove') {
        return new Promise<Response>((_resolve, reject) => {
          init?.signal?.addEventListener('abort', () => {
            removeAborted = true;
            reject(new DOMException('Aborted', 'AbortError'));
          });
        });
      }
      throw new Error(`Unexpected ${input}`);
    }));
    render(panel({ layoutMutationTimeoutMs: 5 }));
    const ui = within(document.body);
    await act(async () => { await vi.advanceTimersByTimeAsync(0); });
    fireEvent.click(ui.getByRole('button', { name: 'Remove image' }));
    await act(async () => { await vi.advanceTimersByTimeAsync(5); });
    expect(removeAborted).toBe(true);
    expect(ui.getByText('Floorplan recovered after removal timeout.')).not.toBeNull();
    expect(ui.getByAltText('Uploaded floorplan')).not.toBeNull();
  });

  it('aborts FileReader work when an upload mutation times out', async () => {
    vi.useFakeTimers();
    class HangingFileReader {
      static instance: HangingFileReader | null = null;
      result: string | ArrayBuffer | null = null;
      onerror: (() => void) | null = null;
      onabort: (() => void) | null = null;
      onload: (() => void) | null = null;
      abort = vi.fn(() => this.onabort?.());
      readAsDataURL = vi.fn();

      constructor() {
        HangingFileReader.instance = this;
      }
    }
    vi.stubGlobal('FileReader', HangingFileReader);
    const initial = layoutReady();
    const fetchMock = vi.fn((input: string, init?: RequestInit) => {
      if (input === '/api/layout' && !init?.method) return Promise.resolve(jsonResponse(200, initial));
      if (input === '/api/floorplan/upload') throw new Error('Upload should wait for the file read.');
      throw new Error(`Unexpected ${input}`);
    });
    stubFetch(fetchMock);
    const { unmount } = render(panel({ layoutMutationTimeoutMs: 5 }));
    const ui = within(document.body);
    await act(async () => { await vi.advanceTimersByTimeAsync(0); });
    fireEvent.change(ui.getByLabelText('Upload image'), { target: { files: [new File(['plan'], 'plan.png', { type: 'image/png' })] } });
    await act(async () => { await Promise.resolve(); });
    expect(HangingFileReader.instance?.readAsDataURL).toHaveBeenCalledTimes(1);
    await act(async () => { await vi.advanceTimersByTimeAsync(5); });
    expect(HangingFileReader.instance?.abort).toHaveBeenCalledTimes(1);
    expect(fetchMock.mock.calls.filter(([input]) => input === '/api/floorplan/upload')).toHaveLength(0);
    unmount();
    expect(vi.getTimerCount()).toBe(0);
  });

  it('ignores an aborted stale layout read after a newer read commits', async () => {
    const resolveReads: Array<(response: Response) => void> = [];
    stubFetch(vi.fn((input: string) => {
      if (input !== '/api/layout') throw new Error(`Unexpected ${input}`);
      return new Promise<Response>((resolve) => { resolveReads.push(resolve); });
    }));
    render(<StrictMode>{panel()}</StrictMode>);
    await waitFor(() => expect(resolveReads).toHaveLength(2));
    const newer = layoutReady({ revision: 2, positions: [{ adva: '1842de524add', x: 0.8, y: 0.7 }] });
    const stale = layoutReady({ revision: 1, positions: [{ adva: '1842de524add', x: 0.1, y: 0.2 }] });
    await act(async () => { resolveReads[1]?.(jsonResponse(200, newer)); });
    const ui = within(document.body);
    await ui.findByRole('button', { name: /Gateway, positioned at x 0.80, y 0.70/i });
    await act(async () => { resolveReads[0]?.(jsonResponse(200, stale)); });
    expect(ui.getByRole('button', { name: /Gateway, positioned at x 0.80, y 0.70/i })).not.toBeNull();
  });

  it('renders exact node coordinate points while keeping node hit targets clamped at edges', async () => {
    const boundary = layoutReady({ positions: [
      { adva: '0102545678c1', x: 1, y: 0 },
      { adva: '1842de524add', x: 0, y: 1 },
    ] });
    stubFetch(vi.fn((input: string) => {
      if (input === '/api/layout') return Promise.resolve(jsonResponse(200, boundary));
      throw new Error(`Unexpected ${input}`);
    }));
    render(panel());
    const gateway = await within(document.body).findByRole('button', { name: /Gateway, positioned at x 0.00, y 1.00/i });
    const remote = within(document.body).getByRole('button', { name: /0102545678c1, positioned at x 1.00, y 0.00/i });
    const gatewayPoint = gateway.parentElement?.querySelector('.floorplan-node-point');
    const remotePoint = remote.parentElement?.querySelector('.floorplan-node-point');
    if (!(gatewayPoint instanceof HTMLElement) || !(remotePoint instanceof HTMLElement)) throw new Error('Expected exact node coordinate points.');
    expect(gateway.style.left).toBe('clamp(28px, 0%, 100% - 28px)');
    expect(gateway.style.top).toBe('clamp(28px, 100%, 100% - 28px)');
    expect(remote.style.left).toBe('clamp(28px, 100%, 100% - 28px)');
    expect(remote.style.top).toBe('clamp(28px, 0%, 100% - 28px)');
    expect(gatewayPoint.style.left).toBe('0%');
    expect(gatewayPoint.style.top).toBe('100%');
    expect(remotePoint.style.left).toBe('100%');
    expect(remotePoint.style.top).toBe('0%');
    expect(gateway.className).toContain('floorplan-node');
    expect(gatewayPoint.className).toContain('floorplan-node-point');
  });

  it('shows stored off-roster positions for individual unplacement and announces capacity exhaustion', async () => {
    const orphan = { adva: 'ffffffffffff', x: 0.1, y: 0.2 };
    const saved = layoutReady({ revision: 1, positions: [] });
    const fetchMock = vi.fn()
      .mockResolvedValueOnce(jsonResponse(200, layoutReady({ positions: [orphan] })))
      .mockResolvedValueOnce(jsonResponse(200, saved));
    stubFetch(fetchMock);
    const { unmount } = render(panel());
    const ui = within(document.body);
    await ui.findByText('Stored, not in current GTT');
    const row = ui.getByText('ffffffffffff').closest('tr');
    if (!row) throw new Error('Expected stored-position row.');
    fireEvent.click(within(row).getByRole('button', { name: 'Unplace' }));
    await ui.findByText('Stored node unplaced.');
    unmount();

    const full = Array.from({ length: 16 }, (_, index) => ({ adva: (index + 1).toString(16).padStart(12, '0'), x: 0.5, y: 0.5 }));
    const fullFetch = vi.fn((input: string, init?: RequestInit) => {
      if (input === '/api/layout' && !init?.method) return Promise.resolve(jsonResponse(200, layoutReady({ positions: full })));
      throw new Error(`Unexpected ${input}`);
    });
    stubFetch(fullFetch);
    render(panel());
    const capacityUi = within(document.body);
    const staging = await capacityUi.findByRole('region', { name: /Unpositioned nodes/i });
    fireEvent.click(within(staging).getAllByRole('button', { name: 'Place at center' })[0]!);
    await capacityUi.findByText('Floorplan position limit reached (16). Unplace a stored position before placing another node.');
    expect(fullFetch.mock.calls.filter(([input]) => input === '/api/layout').filter(([, init]) => (init as RequestInit | undefined)?.method === 'PUT')).toHaveLength(0);
  });

  it('asks for a specific clear-all confirmation without mutating or losing focus when cancelled', async () => {
    const fetchMock = vi.fn((input: string, _init?: RequestInit) => {
      if (input === '/api/layout') return Promise.resolve(jsonResponse(200, layoutReady({ positions: [{ adva: '1842de524add', x: 0.2, y: 0.3 }] })));
      throw new Error(`Unexpected ${input}`);
    });
    stubFetch(fetchMock);
    const confirm = vi.spyOn(window, 'confirm').mockReturnValue(false);
    render(panel());
    const ui = within(document.body);
    const clear = await ui.findByRole('button', { name: 'Clear all positions' });
    clear.focus();
    fireEvent.click(clear);
    expect(confirm).toHaveBeenCalledWith('Clear all 1 stored node position? This removes their floorplan coordinates.');
    expect(fetchMock.mock.calls.filter(([input]) => input === '/api/layout').filter(([, init]) => (init as RequestInit | undefined)?.method === 'PUT')).toHaveLength(0);
    expect(document.activeElement).toBe(clear);
    confirm.mockRestore();
  });

  it('retains coordinates through image upload and removal and restores focus to upload', async () => {
    const image = { sha256: 'a'.repeat(64), mime: 'image/png' as const, width: 800, height: 600, url: `/api/floorplan/${'a'.repeat(64)}` };
    const positions = [{ adva: '1842de524add', x: 0.2, y: 0.3 }];
    let uploadSignal: AbortSignal | undefined;
    let removeSignal: AbortSignal | undefined;
    const fetchMock = vi.fn((input: string, init?: RequestInit) => {
      if (input === '/api/layout' && !init?.method) return Promise.resolve(jsonResponse(200, layoutReady({ positions })));
      if (input === '/api/floorplan/upload') {
        uploadSignal = init?.signal ?? undefined;
        return Promise.resolve(jsonResponse(200, layoutReady({ revision: 1, floorplan: image, positions })));
      }
      if (input === '/api/floorplan/remove') {
        removeSignal = init?.signal ?? undefined;
        return Promise.resolve(jsonResponse(200, layoutReady({ revision: 2, positions })));
      }
      throw new Error(`Unexpected ${input}`);
    });
    stubFetch(fetchMock);
    render(panel());
    const ui = within(document.body);
    const upload = await ui.findByLabelText('Upload image');
    fireEvent.change(upload, { target: { files: [new File(['plan'], 'plan.png', { type: 'image/png' })] } });
    await ui.findByAltText('Uploaded floorplan');
    expect(uploadSignal).toBeDefined();
    expect(ui.getByRole('button', { name: /Gateway, positioned at x 0.20, y 0.30/i })).not.toBeNull();
    fireEvent.click(ui.getByRole('button', { name: 'Remove image' }));
    await ui.findByText('Floorplan removed; node positions retained.');
    expect(removeSignal).toBeDefined();
    await waitFor(() => expect(document.activeElement).toBe(upload));
    expect(ui.getByRole('button', { name: /Gateway, positioned at x 0.20, y 0.30/i })).not.toBeNull();
  });

  it('uses the stable upload fallback when an existing origin refuses focus', async () => {
    const initial = layoutReady({ positions: [{ adva: '1842de524add', x: 0.2, y: 0.3 }] });
    const saved = layoutReady({ revision: 1, positions: [{ adva: '1842de524add', x: 0.21, y: 0.3 }] });
    stubFetch(vi.fn((input: string, init?: RequestInit) => {
      if (input === '/api/layout' && !init?.method) return Promise.resolve(jsonResponse(200, initial));
      if (input === '/api/layout') return Promise.resolve(jsonResponse(200, saved));
      throw new Error(`Unexpected ${input}`);
    }));
    render(panel());
    const ui = within(document.body);
    const node = await ui.findByRole('button', { name: /Gateway, positioned at x 0.20, y 0.30/i });
    Object.defineProperty(node, 'focus', { configurable: true, value: vi.fn() });
    const upload = ui.getByLabelText('Upload image');
    fireEvent.keyDown(node, { key: 'ArrowRight' });
    fireEvent.keyUp(node, { key: 'ArrowRight' });
    await ui.findByText('Node position saved.');
    await waitFor(() => expect(document.activeElement).toBe(upload));
  });

  it('publishes only decoded server coordinates after a deferred position save succeeds', async () => {
    let resolveSave: ((response: Response) => void) | undefined;
    const initial = layoutReady({ positions: [{ adva: '1842de524add', x: 0.2, y: 0.3 }] });
    const committed = layoutReady({ revision: 1, positions: [{ adva: '1842de524add', x: 0.8, y: 0.7 }] });
    const onLayoutChange = vi.fn();
    stubFetch(vi.fn((input: string, init?: RequestInit) => {
      if (input === '/api/layout' && !init?.method) return Promise.resolve(jsonResponse(200, initial));
      if (input === '/api/layout' && init?.method === 'PUT') return new Promise<Response>((resolve) => { resolveSave = resolve; });
      throw new Error(`Unexpected ${input}`);
    }));
    render(panel({ onLayoutChange }));
    const ui = within(document.body);
    const node = await ui.findByRole('button', { name: /Gateway, positioned at x 0.20, y 0.30/i });
    expect(onLayoutChange).toHaveBeenLastCalledWith(initial);
    onLayoutChange.mockClear();

    fireEvent.keyDown(node, { key: 'ArrowRight' });
    fireEvent.keyUp(node, { key: 'ArrowRight' });
    await waitFor(() => expect(resolveSave).toBeDefined());
    expect(onLayoutChange).not.toHaveBeenCalled();

    await act(async () => { resolveSave?.(jsonResponse(200, committed)); });
    await waitFor(() => expect(onLayoutChange).toHaveBeenLastCalledWith(committed));
    expect(onLayoutChange).not.toHaveBeenCalledWith(expect.objectContaining({ positions: [{ adva: '1842de524add', x: 0.21, y: 0.3 }] }));
  });

  it('never publishes double-conflict optimistic positions', async () => {
    let layoutReads = 0;
    let recovery: ((response: Response) => void) | undefined;
    const initial = layoutReady({ positions: [{ adva: '1842de524add', x: 0.2, y: 0.3 }] });
    const firstCurrent = layoutReady({ revision: 1, positions: [{ adva: '1842de524add', x: 0.4, y: 0.4 }] });
    const secondCurrent = layoutReady({ revision: 2, positions: [{ adva: '1842de524add', x: 0.7, y: 0.6 }] });
    const onLayoutChange = vi.fn();
    const fetchMock = vi.fn((input: string, init?: RequestInit) => {
      if (input === '/api/layout' && !init?.method && layoutReads++ === 0) return Promise.resolve(jsonResponse(200, initial));
      if (input === '/api/layout' && init?.method === 'PUT') {
        const puts = fetchMock.mock.calls.filter(([path, options]) => path === '/api/layout' && (options as RequestInit).method === 'PUT').length;
        const current = puts === 1 ? firstCurrent : secondCurrent;
        return Promise.resolve(jsonResponse(409, { schema: 'mind.dashboard.layout.conflict.v1', error: 'revision_conflict', current }));
      }
      if (input === '/api/layout') return new Promise<Response>((resolve) => { recovery = resolve; });
      throw new Error(`Unexpected ${input}`);
    });
    stubFetch(fetchMock);
    render(panel({ onLayoutChange, layoutMutationTimeoutMs: 5 }));
    const ui = within(document.body);
    const staging = await ui.findByRole('region', { name: /Unpositioned nodes/i });
    await ui.findByText('Floorplan coordinates loaded. No image uploaded.');
    onLayoutChange.mockClear();
    fireEvent.click(within(staging).getAllByRole('button', { name: 'Place at center' })[0]!);
    await waitFor(() => expect(fetchMock.mock.calls.filter(([path]) => path === '/api/layout')).toHaveLength(4));
    expect(onLayoutChange).not.toHaveBeenCalled();
    if (!recovery) throw new Error('Expected a recovery GET.');
    await act(async () => { recovery?.(jsonResponse(200, secondCurrent)); });
    await waitFor(() => expect(onLayoutChange).toHaveBeenLastCalledWith(secondCurrent));
    expect(onLayoutChange.mock.calls.flat()).not.toContainEqual(expect.objectContaining({ positions: expect.arrayContaining([
      expect.objectContaining({ adva: '0102545678c1', x: 0.5, y: 0.5 }),
    ]) }));
  });

  it("does not publish a failed mutation's optimistic coordinates before recovery commits", async () => {
    let reads = 0;
    let recover: ((response: Response) => void) | undefined;
    const initial = layoutReady({ positions: [{ adva: '1842de524add', x: 0.2, y: 0.3 }] });
    const onLayoutChange = vi.fn();
    stubFetch(vi.fn((input: string, init?: RequestInit) => {
      if (input === '/api/layout' && !init?.method && reads++ === 0) return Promise.resolve(jsonResponse(200, initial));
      if (input === '/api/layout' && init?.method === 'PUT') return Promise.resolve(jsonResponse(503, { schema: 'mind.dashboard.error.v1', accepted: false, error: 'storage_unavailable' }));
      if (input === '/api/layout') return new Promise<Response>((resolve) => { recover = resolve; });
      throw new Error(`Unexpected ${input}`);
    }));
    render(panel({ onLayoutChange }));
    const ui = within(document.body);
    fireEvent.click(await ui.findByRole('button', { name: 'Unplace' }));
    onLayoutChange.mockClear();
    await ui.findByText(/Floorplan save failed: Layout request failed: storage_unavailable/i);
    expect(onLayoutChange).not.toHaveBeenCalled();
    if (!recover) throw new Error('Expected a recovery GET.');
    await act(async () => { recover?.(jsonResponse(200, initial)); });
    await waitFor(() => expect(onLayoutChange).toHaveBeenLastCalledWith(initial));
    expect(onLayoutChange).not.toHaveBeenCalledWith(expect.objectContaining({ positions: [] }));
  });

  it('revokes the confirmed layout when a current recovery GET decodes corrupt state', async () => {
    let layoutReads = 0;
    const initial = layoutReady({ positions: [{ adva: '1842de524add', x: 0.2, y: 0.3 }] });
    const corrupt = {
      schema: 'mind.dashboard.layout.v1', status: 'corrupt', error: 'corrupt_state', revision: 1, floorplan: null, positions: [],
    } as const;
    const onLayoutChange = vi.fn();
    stubFetch(vi.fn((input: string, init?: RequestInit) => {
      if (input === '/api/layout' && !init?.method) {
        return Promise.resolve(jsonResponse(200, layoutReads++ === 0 ? initial : corrupt));
      }
      if (input === '/api/layout' && init?.method === 'PUT') {
        return Promise.resolve(jsonResponse(503, { schema: 'mind.dashboard.error.v1', accepted: false, error: 'storage_unavailable' }));
      }
      throw new Error(`Unexpected ${input}`);
    }));
    render(panel({ onLayoutChange }));
    const ui = within(document.body);
    await ui.findByRole('button', { name: 'Unplace' });
    onLayoutChange.mockClear();
    fireEvent.click(ui.getByRole('button', { name: 'Unplace' }));

    await ui.findByText('Floorplan state is corrupt. Recover storage before editing.');
    expect(onLayoutChange).toHaveBeenLastCalledWith(null);
  });

  it('does not publish an aborted mutation', async () => {
    let signal: AbortSignal | undefined;
    const initial = layoutReady({ positions: [{ adva: '1842de524add', x: 0.2, y: 0.3 }] });
    const onLayoutChange = vi.fn();
    stubFetch(vi.fn((input: string, init?: RequestInit) => {
      if (input === '/api/layout' && !init?.method) return Promise.resolve(jsonResponse(200, initial));
      if (input === '/api/layout' && init?.method === 'PUT') {
        signal = init?.signal ?? undefined;
        return new Promise<Response>(() => undefined);
      }
      throw new Error(`Unexpected ${input}`);
    }));
    const { unmount } = render(panel({ onLayoutChange }));
    const ui = within(document.body);
    fireEvent.click(await ui.findByRole('button', { name: 'Unplace' }));
    await waitFor(() => expect(signal).toBeDefined());
    onLayoutChange.mockClear();
    unmount();
    expect(signal?.aborted).toBe(true);
    expect(onLayoutChange).not.toHaveBeenCalled();
  });

  it('ignores stale StrictMode reads without creating a layout callback loop', async () => {
    const resolveReads: Array<(response: Response) => void> = [];
    const onLayoutChange = vi.fn();
    stubFetch(vi.fn((input: string) => {
      if (input !== '/api/layout') throw new Error(`Unexpected ${input}`);
      return new Promise<Response>((resolve) => { resolveReads.push(resolve); });
    }));
    render(<StrictMode>{panel({ onLayoutChange })}</StrictMode>);
    await waitFor(() => expect(resolveReads).toHaveLength(2));
    const committed = layoutReady({ revision: 2, positions: [{ adva: '1842de524add', x: 0.8, y: 0.7 }] });
    const stale = layoutReady({ revision: 1, positions: [{ adva: '1842de524add', x: 0.1, y: 0.2 }] });

    await act(async () => { resolveReads[1]?.(jsonResponse(200, committed)); });
    await waitFor(() => expect(onLayoutChange).toHaveBeenCalledTimes(1));
    await act(async () => { resolveReads[0]?.(jsonResponse(200, stale)); });
    expect(onLayoutChange).toHaveBeenCalledTimes(1);
    expect(onLayoutChange).toHaveBeenLastCalledWith(committed);
  });

  it('anchors incident points at exact normalized edges while offsetting only their labels', async () => {
    const record = eventRecord({ wearable: 7, packet: '000007' });
    const logicalEvent: LogicalEvent = {
      key: '7:000007',
      fingerprint: '1:3:75:2400:86:42',
      record,
      evidence: [],
      evidenceOverflowCount: 0,
      evidenceSaturated: false,
      conflict: false,
      firstReceivedAt: 0,
      collectUntil: 2_000,
    };
    const views: readonly IncidentLocalizationView[] = [
      {
        logicalEvent,
        status: 'ballpark',
        contributorIds: ['a', 'b', 'c'],
        contributorCount: 3,
        geometryWarning: false,
        normalizedSpread: null,
        x: 0,
        y: 1,
      },
      {
        logicalEvent: { ...logicalEvent, key: '8:000008', record: eventRecord({ wearable: 8, packet: '000008' }) },
        status: 'insufficient',
        contributorIds: ['a', 'b'],
        contributorCount: 2,
      },
    ];
    stubFetch(vi.fn((input: string) => {
      if (input === '/api/layout') return Promise.resolve(jsonResponse(200, layoutReady({ positions: [{ adva: '1842de524add', x: 0.5, y: 0.5 }] })));
      throw new Error(`Unexpected ${input}`);
    }));

    render(panel({ incidentViews: views }));
    const marker = await within(document.body).findByRole('img', { name: /Wearable 7, packet 000007: Ballpark at normalized x 0.00, y 1.00/i });
    expect((marker as HTMLElement).style.left).toBe('0%');
    expect((marker as HTMLElement).style.top).toBe('100%');
    expect(marker.querySelector('.floorplan-marker-label')?.className).toContain('marker-east');
    expect(marker.querySelector('.floorplan-marker-label')?.className).toContain('marker-north');
    const node = within(document.body).getByRole('button', { name: /Gateway, positioned at x 0.50, y 0.50/i });
    const nodePoint = node.parentElement?.querySelector('.floorplan-node-point');
    const incidentPoint = marker.querySelector('.floorplan-marker-point');
    expect(nodePoint?.className).toContain('floorplan-node-point');
    expect(nodePoint?.className).not.toContain('floorplan-marker-point');
    expect(incidentPoint?.className).toContain('floorplan-marker-point');
    expect(incidentPoint?.className).not.toContain('floorplan-node-point');
    expect(within(document.body).getByText('2 of 3 required')).not.toBeNull();
  });
});
