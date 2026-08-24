import { useCallback, useEffect, useRef, useState } from 'react';
import type { GttEntry, GttSnapshot, HealthDevice, RootRecord } from '../lib/api';
import { postGtt } from '../lib/api';
import { formatAge } from '../lib/format';
import { authoritativeRootFor, eligibleTopologySources, nodeIdentity, physicalDevices } from '../state/devices';

export const GTT_REFRESH_INTERVAL_MS = 10_000;
export const GTT_HTTP_TIMEOUT_MS = 5_000;
export const GTT_CONFIRMATION_TIMEOUT_MS = 10_000;
export const GTT_STALE_AFTER_MS = 15_000;

type GttQueryPhase = 'idle' | 'sending' | 'waiting' | 'ready' | 'error';

interface GttQueryState {
  source: number | null;
  epoch: number;
  phase: GttQueryPhase;
  baselineGeneration: number;
  completedGeneration?: number;
  message?: string;
}

interface ActiveGttRequest {
  id: number;
  device: number;
  epoch: number;
  baselineGeneration: number;
  controller: AbortController;
  httpTimer: number;
  confirmationTimer: number;
}

interface DisplayedGtt {
  source: number;
  epoch: number;
  snapshot: GttSnapshot;
}

export interface GttEntryGroup {
  key: string;
  hopLabel: string;
  freshnessLabel: string;
  entries: GttEntry[];
}

interface TopologyPanelProps {
  devices: HealthDevice[];
  healthEpoch: number | null;
  currentEpoch: number;
  healthCurrentCursor: number | null;
  rootRecords: Record<number, RootRecord>;
  now: number;
  refreshIntervalMs?: number;
  httpTimeoutMs?: number;
  confirmationTimeoutMs?: number;
}

function messageFor(error: unknown): string {
  return error instanceof Error ? error.message : 'Unknown bridge error.';
}

function label(value: string): string {
  return value.replaceAll('_', ' ');
}

function hopLabel(entry: GttEntry): string {
  if (entry.hop_state === 'known') return `Hop ${entry.hop}`;
  return `Hop ${label(entry.hop_state)}`;
}

function freshnessLabel(entry: GttEntry): string {
  return label(entry.freshness);
}

function stateValue(value: number, state: GttEntry['serial_state']): string {
  if (state === 'known') return String(value);
  if (state === 'not_applicable') return 'Not applicable';
  return `Unknown (raw ${value})`;
}

function departedLabel(departed: GttEntry['departed']): string {
  switch (departed) {
    case 'true': return 'Yes';
    case 'false': return 'No';
    case 'unknown': return 'Unknown';
    case 'not_applicable': return 'Not applicable';
  }
}

export function isGttStale(snapshot: GttSnapshot, now: number): boolean {
  return now - snapshot.completed_at_ms >= GTT_STALE_AFTER_MS;
}

export function groupGttEntries(entries: GttEntry[]): GttEntryGroup[] {
  const groups = new Map<string, GttEntryGroup>();
  for (const entry of entries) {
    const key = `${entry.hop_state}:${entry.hop}:${entry.freshness}`;
    const group = groups.get(key) ?? {
      key,
      hopLabel: hopLabel(entry),
      freshnessLabel: freshnessLabel(entry),
      entries: [],
    };
    group.entries.push(entry);
    groups.set(key, group);
  }
  return [...groups.values()]
    .sort((left, right) => left.hopLabel.localeCompare(right.hopLabel) || left.freshnessLabel.localeCompare(right.freshnessLabel))
    .map((group) => ({ ...group, entries: [...group.entries].sort((left, right) => left.index - right.index) }));
}

function GttEntryCard({ entry, knownNode }: { entry: GttEntry; knownNode?: number }) {
  return (
    <article className={`compact-card gtt-entry-card freshness-${entry.freshness}`}>
      <div className="card-top">
        <div>
          <h4>{knownNode === undefined ? <>AdvA <code>{entry.adva}</code></> : `Node ${knownNode}`}</h4>
          <p>{knownNode === undefined ? `Entry ${entry.index}` : <>AdvA <code>{entry.adva}</code> · Entry {entry.index}</>} · departed: {departedLabel(entry.departed)}</p>
        </div>
        <span className="status-chip muted">{freshnessLabel(entry)}</span>
      </div>
      <dl className="metric-row">
        <div><dt>Serial</dt><dd>{stateValue(entry.serial, entry.serial_state)}</dd></div>
        <div><dt>Hop</dt><dd>{stateValue(entry.hop, entry.hop_state)}</dd></div>
        <div><dt>Departed</dt><dd>{departedLabel(entry.departed)}</dd></div>
      </dl>
      <details className="firmware-values">
        <summary>Firmware record values</summary>
        <dl>
          <div><dt>Last</dt><dd>{entry.last}</dd></div>
          <div><dt>Soft</dt><dd>{entry.soft}</dd></div>
          <div><dt>Hard</dt><dd>{entry.hard}</dd></div>
          <div><dt>Departed deadline</dt><dd>{entry.departed_deadline}</dd></div>
        </dl>
      </details>
    </article>
  );
}

export function TopologyPanel({
  devices,
  healthEpoch,
  currentEpoch,
  healthCurrentCursor,
  rootRecords,
  now,
  refreshIntervalMs = GTT_REFRESH_INTERVAL_MS,
  httpTimeoutMs = GTT_HTTP_TIMEOUT_MS,
  confirmationTimeoutMs = GTT_CONFIRMATION_TIMEOUT_MS,
}: TopologyPanelProps) {
  const eligible = eligibleTopologySources(devices, healthEpoch, currentEpoch, healthCurrentCursor, rootRecords);
  const eligibleKey = eligible.map((device) => device.device).join(',');
  const [selectedDevice, setSelectedDevice] = useState<number | null>(null);
  const [query, setQuery] = useState<GttQueryState>(() => ({ source: null, epoch: currentEpoch, phase: 'idle', baselineGeneration: 0 }));
  const [displayedGtt, setDisplayedGtt] = useState<DisplayedGtt | null>(null);
  const requestId = useRef(0);
  const activeRequest = useRef<ActiveGttRequest | null>(null);
  const refreshTimer = useRef<number | undefined>(undefined);
  const eligibleRef = useRef(eligible);
  const selectedRef = useRef(selectedDevice);
  const currentEpochRef = useRef(currentEpoch);
  const healthEpochRef = useRef(healthEpoch);
  eligibleRef.current = eligible;
  selectedRef.current = selectedDevice;
  currentEpochRef.current = currentEpoch;
  healthEpochRef.current = healthEpoch;

  const clearRefreshTimer = useCallback(() => {
    if (refreshTimer.current !== undefined) {
      window.clearTimeout(refreshTimer.current);
      refreshTimer.current = undefined;
    }
  }, []);

  const cancelActiveRequest = useCallback(() => {
    const active = activeRequest.current;
    if (!active) return;
    window.clearTimeout(active.httpTimer);
    window.clearTimeout(active.confirmationTimer);
    active.controller.abort();
    activeRequest.current = null;
  }, []);

  const startGttRequest = useCallback((device: number) => {
    if (activeRequest.current) return;
    const source = eligibleRef.current.find((candidate) => candidate.device === device);
    if (!source) return;
    clearRefreshTimer();
    const epoch = currentEpochRef.current;
    const baselineGeneration = healthEpochRef.current === epoch ? source.gtt?.generation ?? 0 : 0;
    const id = ++requestId.current;
    const controller = new AbortController();
    const active: ActiveGttRequest = {
      id,
      device,
      epoch,
      baselineGeneration,
      controller,
      httpTimer: 0,
      confirmationTimer: 0,
    };
    const finishWithError = (message: string) => {
      if (activeRequest.current?.id !== id) return;
      window.clearTimeout(active.httpTimer);
      window.clearTimeout(active.confirmationTimer);
      activeRequest.current = null;
      setQuery({ source: device, epoch, phase: 'error', baselineGeneration, message });
    };
    active.httpTimer = window.setTimeout(() => {
      controller.abort();
      finishWithError('GTT serial-write request timed out; retry this source.');
    }, httpTimeoutMs);
    active.confirmationTimer = window.setTimeout(() => {
      controller.abort();
      finishWithError('No newer GTT generation arrived in time; retry this source.');
    }, confirmationTimeoutMs);
    activeRequest.current = active;
    setQuery({ source: device, epoch, phase: 'sending', baselineGeneration, message: 'Requesting GTT serial write…' });
    void postGtt(device, controller.signal)
      .then(() => {
        if (activeRequest.current?.id !== id || activeRequest.current.epoch !== epoch) return;
        window.clearTimeout(active.httpTimer);
        setQuery({ source: device, epoch, phase: 'waiting', baselineGeneration, message: 'Serial write accepted; waiting for a newer GTT generation.' });
      })
      .catch((error: unknown) => {
        if (activeRequest.current?.id !== id || activeRequest.current.epoch !== epoch) return;
        finishWithError(messageFor(error));
      });
  }, [clearRefreshTimer, confirmationTimeoutMs, httpTimeoutMs]);

  const scheduleRefresh = useCallback((device: number) => {
    clearRefreshTimer();
    refreshTimer.current = window.setTimeout(() => {
      refreshTimer.current = undefined;
      if (selectedRef.current === device) startGttRequest(device);
    }, refreshIntervalMs);
  }, [clearRefreshTimer, refreshIntervalMs, startGttRequest]);

  useEffect(() => {
    setSelectedDevice((current) => eligible.some((device) => device.device === current)
      ? current
      : eligible[0]?.device ?? null);
  }, [eligibleKey]);

  useEffect(() => {
    cancelActiveRequest();
    clearRefreshTimer();
    if (selectedDevice === null) {
      setQuery({ source: null, epoch: currentEpoch, phase: 'idle', baselineGeneration: 0 });
      setDisplayedGtt(null);
      return undefined;
    }
    setDisplayedGtt((current) => current?.source === selectedDevice && current.epoch === currentEpoch ? current : null);
    startGttRequest(selectedDevice);
    return () => {
      cancelActiveRequest();
      clearRefreshTimer();
    };
  }, [cancelActiveRequest, clearRefreshTimer, currentEpoch, selectedDevice, startGttRequest]);

  const selectedSource = eligible.find((device) => device.device === selectedDevice) ?? null;
  const snapshot = healthEpoch === currentEpoch ? selectedSource?.gtt ?? null : null;
  useEffect(() => {
    const active = activeRequest.current;
    if (!active
      || active.device !== selectedDevice
      || active.epoch !== currentEpoch
      || !snapshot
      || snapshot.generation <= active.baselineGeneration) return;
    window.clearTimeout(active.httpTimer);
    window.clearTimeout(active.confirmationTimer);
    activeRequest.current = null;
    setQuery({
      source: active.device,
      epoch: active.epoch,
      phase: 'ready',
      baselineGeneration: active.baselineGeneration,
      completedGeneration: snapshot.generation,
      message: 'New GTT generation received.',
    });
    setDisplayedGtt({ source: active.device, epoch: active.epoch, snapshot });
    scheduleRefresh(active.device);
  }, [currentEpoch, scheduleRefresh, selectedDevice, snapshot]);

  const sourceRoot = selectedSource
    ? authoritativeRootFor(selectedSource, healthEpoch, currentEpoch, healthCurrentCursor, rootRecords[selectedSource.device])
    : null;
  const sourceIdentity = selectedSource ? nodeIdentity(selectedSource, sourceRoot) : null;
  const completedSnapshot = displayedGtt?.source === selectedDevice
    && displayedGtt.epoch === currentEpoch
    ? displayedGtt.snapshot
    : null;
  const stale = completedSnapshot ? isGttStale(completedSnapshot, now) : false;
  const groups = completedSnapshot ? groupGttEntries(completedSnapshot.entries) : [];
  const knownNodesByAdvA = new Map<string, number>();
  for (const device of physicalDevices(devices)) {
    if (!device.connected) continue;
    const root = authoritativeRootFor(device, healthEpoch, currentEpoch, healthCurrentCursor, rootRecords[device.device]);
    if (root) knownNodesByAdvA.set(root.local, root.node);
  }

  return (
    <section className="panel topology-panel" aria-labelledby="topology-heading">
      <div className="section-heading">
        <div>
          <p className="eyebrow">Selected-source GTT interrogation</p>
          <h2 id="topology-heading">Topology</h2>
        </div>
        {selectedSource && <span className={`status-chip ${query.phase === 'error' ? 'danger' : query.phase === 'ready' && !stale ? 'good' : 'warning'}`}>{query.phase}</span>}
      </div>
      {eligible.length === 0 ? (
        <p className="empty">No connected physical device is eligible to provide topology. Reconnect a device or wait for an authoritative root status.</p>
      ) : (
        <>
          {eligible.length > 1 ? (
            <label className="source-selector">
              <span>Topology source</span>
              <select value={selectedDevice ?? ''} onChange={(event) => setSelectedDevice(Number(event.currentTarget.value))}>
                {eligible.map((device) => {
                  const root = authoritativeRootFor(device, healthEpoch, currentEpoch, healthCurrentCursor, rootRecords[device.device]);
                  return <option key={device.device} value={device.device}>{nodeIdentity(device, root).primary} · Device {device.device}</option>;
                })}
              </select>
            </label>
          ) : sourceIdentity && (
            <p className="source-identity"><strong>Topology source: {sourceIdentity.primary}</strong><span>{sourceIdentity.secondary}</span></p>
          )}
          {eligible.length > 1 && sourceIdentity && <p className="source-identity"><strong>Selected source: {sourceIdentity.primary}</strong><span>{sourceIdentity.secondary}</span></p>}
          <div className="topology-status" role="status" aria-live="polite">
            <p>{query.message ?? 'Select a source to request GTT.'}</p>
            <button
              type="button"
              className="secondary compact-action"
              disabled={selectedDevice === null || query.phase === 'sending' || query.phase === 'waiting'}
              onClick={() => {
                if (selectedDevice !== null) startGttRequest(selectedDevice);
              }}
            >
              {query.phase === 'sending' || query.phase === 'waiting' ? 'GTT request pending' : 'Retry GTT'}
            </button>
          </div>
          {completedSnapshot && (
            <>
              <dl className={`metric-row topology-metrics${stale ? ' stale' : ''}`}>
                <div><dt>Host completion</dt><dd>{formatAge(Math.max(0, now - completedSnapshot.completed_at_ms))}{stale ? ' · stale' : ''}</dd></div>
                <div><dt>Query ID</dt><dd>{completedSnapshot.query_at_ms}</dd></div>
                <div><dt>Members</dt><dd>{completedSnapshot.entry_count}</dd></div>
                <div><dt>Nondeparted</dt><dd>{completedSnapshot.nondeparted_count}</dd></div>
              </dl>
              {groups.length === 0 ? (
                <p className="empty">The selected source reported an empty GTT membership snapshot.</p>
              ) : (
                <div className="gtt-groups" aria-label="GTT membership grouped by hop and freshness">
                  {groups.map((group) => (
                    <section className="gtt-group" key={group.key} aria-labelledby={`gtt-group-${group.key}`}>
                      <h3 id={`gtt-group-${group.key}`}>{group.hopLabel} · {group.freshnessLabel} <span className="count">{group.entries.length}</span></h3>
                      <div className="topology-grid">
                        {group.entries.map((entry) => <GttEntryCard key={entry.adva} entry={entry} knownNode={knownNodesByAdvA.get(entry.adva)} />)}
                      </div>
                    </section>
                  ))}
                </div>
              )}
            </>
          )}
        </>
      )}
    </section>
  );
}
