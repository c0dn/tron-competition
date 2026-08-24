import type {
  CommandRecord,
  EventRecord,
  EventsResponse,
  HealthResponse,
  RootRecord,
} from '../lib/api';

export const EVIDENCE_LIMIT = 16;
export const FEED_LIMIT = 256;
export const PROCESSED_CURSOR_LIMIT = 256;
export const STALE_AFTER_MS = 15_000;

export interface RequestState {
  phase: 'loading' | 'ready' | 'error';
  lastSuccessAt?: number;
  error?: string;
}

export interface Evidence {
  root: string;
  observer: string;
  path: EventRecord['path'];
  device: number;
}

export interface LogicalEvent {
  key: string;
  record: EventRecord;
  evidence: Evidence[];
  evidenceSaturated: boolean;
  conflict: boolean;
}

export interface LogicalTombstone {
  fingerprint: string;
  conflict: boolean;
}

export interface LatestWearable {
  record: EventRecord;
  receivedAt: number;
  epoch: number;
}

export interface PendingRootRequest {
  desired: boolean;
  requestId: number;
  baselineCursor: number;
  baselineEpoch: number;
  phase: 'writing' | 'confirming';
}

export interface DashboardState {
  epoch: number;
  eventCursor: number;
  eventsRequest: RequestState;
  healthRequest: RequestState;
  health: HealthResponse | null;
  healthEpoch: number | null;
  logicalEvents: Record<string, LogicalEvent>;
  tombstones: Record<string, LogicalTombstone>;
  processedCursors: number[];
  feedKeys: string[];
  wearables: Record<number, LatestWearable>;
  rootRecords: Record<number, RootRecord>;
  commandRecords: Record<number, CommandRecord>;
  pendingRoot: Record<number, PendingRootRequest | undefined>;
  announcement: string;
  conflictCount: number;
  gapCount: number;
  lastGap: Pick<EventsResponse, 'oldest_cursor' | 'current_cursor'> | null;
}

export const initialState: DashboardState = {
  epoch: 0,
  eventCursor: 0,
  eventsRequest: { phase: 'loading' },
  healthRequest: { phase: 'loading' },
  health: null,
  healthEpoch: null,
  logicalEvents: {},
  tombstones: {},
  processedCursors: [],
  feedKeys: [],
  wearables: {},
  rootRecords: {},
  commandRecords: {},
  pendingRoot: {},
  announcement: '',
  conflictCount: 0,
  gapCount: 0,
  lastGap: null,
};

export type Action =
  | { type: 'eventsReceived'; page: EventsResponse; receivedAt: number; epoch: number }
  | { type: 'epochReset' }
  | { type: 'healthReceived'; health: HealthResponse; receivedAt: number; epoch: number }
  | { type: 'requestFailed'; source: 'events' | 'health'; message: string }
  | { type: 'rootPending'; device: number; active: boolean; requestId: number; baselineCursor: number; baselineEpoch: number }
  | { type: 'rootAccepted'; device: number; command: 'on' | 'off'; requestId: number }
  | { type: 'rootFailed'; device: number; requestId: number; message: string }
  | { type: 'rootConfirmationTimedOut'; device: number; requestId: number };

function logicalKey(event: EventRecord): string {
  return `${event.wearable}:${event.packet}`;
}

function payloadFingerprint(event: EventRecord): string {
  return [event.schema, event.event, event.confidence, event.svm, event.mic, event.seq].join(':');
}

function sameEvidence(left: Evidence, right: Evidence): boolean {
  return left.root === right.root
    && left.observer === right.observer
    && left.path === right.path
    && left.device === right.device;
}

function addEvidence(logical: LogicalEvent, event: EventRecord): LogicalEvent {
  const evidence: Evidence = {
    root: event.root,
    observer: event.observer,
    path: event.path,
    device: event.device,
  };
  if (logical.evidence.some((item) => sameEvidence(item, evidence))) return logical;
  if (logical.evidence.length >= EVIDENCE_LIMIT) {
    return { ...logical, evidenceSaturated: true };
  }
  return { ...logical, evidence: [...logical.evidence, evidence] };
}

function rememberCursor(cursors: number[], cursor: number): number[] {
  if (cursors.includes(cursor)) return cursors;
  return [...cursors, cursor].slice(-PROCESSED_CURSOR_LIMIT);
}

function activeFeed(
  feedKeys: string[],
  logicalEvents: Record<string, LogicalEvent>,
  key: string,
  logical: LogicalEvent,
): Pick<DashboardState, 'feedKeys' | 'logicalEvents'> {
  const nextKeys = [key, ...feedKeys.filter((existing) => existing !== key)].slice(0, FEED_LIMIT);
  const nextLogicalEvents = { ...logicalEvents, [key]: logical };
  for (const existing of Object.keys(nextLogicalEvents)) {
    if (!nextKeys.includes(existing)) delete nextLogicalEvents[existing];
  }
  return { feedKeys: nextKeys, logicalEvents: nextLogicalEvents };
}

function advanceCursor(current: number, page: EventsResponse): number {
  const greatestRecordCursor = page.events.reduce((greatest, record) => Math.max(greatest, record.cursor), current);
  return page.events.length === 0 ? Math.max(current, page.current_cursor) : greatestRecordCursor;
}

function updateWearable(
  wearables: Record<number, LatestWearable>,
  event: EventRecord,
  receivedAt: number,
  epoch: number,
): Record<number, LatestWearable> {
  const current = wearables[event.wearable];
  if (current && (current.epoch > epoch || (current.epoch === epoch && current.record.cursor >= event.cursor))) {
    return wearables;
  }
  return { ...wearables, [event.wearable]: { record: event, receivedAt, epoch } };
}

function clearPendingRoot(
  pendingRoot: DashboardState['pendingRoot'],
  device: number,
  requestId: number,
): DashboardState['pendingRoot'] {
  const pending = pendingRoot[device];
  if (!pending || pending.requestId !== requestId) return pendingRoot;
  const next = { ...pendingRoot };
  delete next[device];
  return next;
}

interface PendingRootResolution {
  pendingRoot: DashboardState['pendingRoot'];
  announcement?: string;
}

function resolvePendingRoot(
  pendingRoot: DashboardState['pendingRoot'],
  device: number,
  active: boolean,
  cursor: number,
  epoch: number,
  source: 'an authoritative root record' | 'the authoritative health snapshot',
): PendingRootResolution {
  const pending = pendingRoot[device];
  if (!pending
    || pending.baselineEpoch !== epoch
    || cursor <= pending.baselineCursor) {
    return { pendingRoot };
  }
  const desired = pending.desired ? 'ON' : 'OFF';
  const reported = active ? 'ON' : 'OFF';
  return {
    pendingRoot: clearPendingRoot(pendingRoot, device, pending.requestId),
    announcement: active === pending.desired
      ? `Device ${device} confirmed ROOT ${reported} from ${source}.`
      : `Device ${device} reported ROOT ${reported} from ${source}; requested ROOT ${desired} did not match.`,
  };
}

function rootResolvesPending(
  pendingRoot: DashboardState['pendingRoot'],
  record: RootRecord,
  epoch: number,
): PendingRootResolution {
  return resolvePendingRoot(
    pendingRoot,
    record.device,
    record.role === 'root',
    record.cursor,
    epoch,
    'an authoritative root record',
  );
}

function commandRejectsPending(
  pendingRoot: DashboardState['pendingRoot'],
  record: CommandRecord,
  epoch: number,
): DashboardState['pendingRoot'] {
  const pending = pendingRoot[record.device];
  const expectedCommand = pending?.desired ? 'on' : 'off';
  const terminalFailure = record.status === 'busy' || record.status === 'rejected' || record.status === 'malformed' || record.status === 'overflow';
  if (!pending
    || pending.baselineEpoch !== epoch
    || record.cursor <= pending.baselineCursor
    || record.command !== expectedCommand
    || !terminalFailure) {
    return pendingRoot;
  }
  return clearPendingRoot(pendingRoot, record.device, pending.requestId);
}

function healthResolvesPending(state: DashboardState, health: HealthResponse, epoch: number): PendingRootResolution {
  if (epoch !== state.epoch) return { pendingRoot: state.pendingRoot };
  let pendingRoot = state.pendingRoot;
  let announcement: string | undefined;
  for (const device of health.devices) {
    if (device.device !== device.owner_device || !device.root) continue;
    const resolution = resolvePendingRoot(
      pendingRoot,
      device.device,
      device.root.role === 'root',
      device.root.cursor,
      epoch,
      'the authoritative health snapshot',
    );
    pendingRoot = resolution.pendingRoot;
    announcement = resolution.announcement ?? announcement;
  }
  return { pendingRoot, announcement };
}

function applyEvent(
  state: DashboardState,
  event: EventRecord,
  receivedAt: number,
  epoch: number,
): Pick<DashboardState, 'logicalEvents' | 'tombstones' | 'feedKeys' | 'wearables' | 'conflictCount'> {
  const key = logicalKey(event);
  const fingerprint = payloadFingerprint(event);
  const tombstone = state.tombstones[key];
  const wearables = updateWearable(state.wearables, event, receivedAt, epoch);

  if (!tombstone) {
    const logical: LogicalEvent = {
      key,
      record: event,
      evidence: [{ root: event.root, observer: event.observer, path: event.path, device: event.device }],
      evidenceSaturated: false,
      conflict: false,
    };
    const active = activeFeed(state.feedKeys, state.logicalEvents, key, logical);
    return {
      ...active,
      tombstones: { ...state.tombstones, [key]: { fingerprint, conflict: false } },
      wearables,
      conflictCount: state.conflictCount,
    };
  }

  const activeLogical = state.logicalEvents[key];
  const conflict = tombstone.fingerprint !== fingerprint;
  const nextConflict = tombstone.conflict || conflict;
  const logical: LogicalEvent = activeLogical
    ? { ...addEvidence(activeLogical, event), conflict: nextConflict }
    : {
      key,
      record: event,
      evidence: [{ root: event.root, observer: event.observer, path: event.path, device: event.device }],
      evidenceSaturated: false,
      conflict: nextConflict,
    };
  const active = activeFeed(state.feedKeys, state.logicalEvents, key, logical);
  return {
    ...active,
    tombstones: { ...state.tombstones, [key]: { ...tombstone, conflict: nextConflict } },
    wearables,
    conflictCount: state.conflictCount + (conflict ? 1 : 0),
  };
}

export function reducer(state: DashboardState, action: Action): DashboardState {
  switch (action.type) {
    case 'eventsReceived': {
      if (action.epoch !== state.epoch) return state;
      let next: DashboardState = {
        ...state,
        eventCursor: advanceCursor(state.eventCursor, action.page),
        eventsRequest: { phase: 'ready', lastSuccessAt: action.receivedAt },
        gapCount: state.gapCount + (action.page.gap ? 1 : 0),
        lastGap: action.page.gap
          ? { oldest_cursor: action.page.oldest_cursor, current_cursor: action.page.current_cursor }
          : state.lastGap,
      };
      let rootResolutionAnnouncement: string | undefined;
      const records = [...action.page.events].sort((left, right) => left.cursor - right.cursor);
      for (const record of records) {
        if (next.processedCursors.includes(record.cursor)) continue;
        next = { ...next, processedCursors: rememberCursor(next.processedCursors, record.cursor) };
        if (record.kind === 'event') {
          next = { ...next, ...applyEvent(next, record, action.receivedAt, action.epoch) };
        } else if (record.kind === 'root') {
          const existing = next.rootRecords[record.device];
          if (!existing || existing.cursor < record.cursor) {
            const resolution = rootResolvesPending(next.pendingRoot, record, action.epoch);
            rootResolutionAnnouncement = resolution.announcement ?? rootResolutionAnnouncement;
            next = {
              ...next,
              rootRecords: { ...next.rootRecords, [record.device]: record },
              pendingRoot: resolution.pendingRoot,
              announcement: resolution.announcement ?? next.announcement,
            };
          }
        } else {
          const existing = next.commandRecords[record.device];
          if (!existing || existing.cursor < record.cursor) {
            const pendingRoot = commandRejectsPending(next.pendingRoot, record, action.epoch);
            const request = next.pendingRoot[record.device];
            const announcement = record.command === 'gtt'
              ? next.announcement
              : request && pendingRoot !== next.pendingRoot
                ? `Device ${record.device} firmware command ROOT ${record.command.toUpperCase()} failed: ${record.status}.`
                : `Device ${record.device} reported ROOT ${record.command.toUpperCase()}: ${record.status}.`;
            next = {
              ...next,
              commandRecords: { ...next.commandRecords, [record.device]: record },
              pendingRoot,
              announcement,
            };
          }
        }
      }
      return rootResolutionAnnouncement ? { ...next, announcement: rootResolutionAnnouncement } : next;
    }
    case 'epochReset':
      return {
        ...state,
        epoch: state.epoch + 1,
        eventCursor: 0,
        processedCursors: [],
        rootRecords: {},
        commandRecords: {},
        pendingRoot: {},
        lastGap: null,
        announcement: 'Bridge cursor restarted; root confirmations were reset and current records are refreshing.',
      };
    case 'healthReceived': {
      const resolution = healthResolvesPending(state, action.health, action.epoch);
      return {
        ...state,
        health: action.health,
        healthEpoch: action.epoch,
        healthRequest: { phase: 'ready', lastSuccessAt: action.receivedAt },
        pendingRoot: resolution.pendingRoot,
        announcement: resolution.announcement ?? state.announcement,
      };
    }
    case 'requestFailed': {
      const request = action.source === 'events' ? state.eventsRequest : state.healthRequest;
      const nextRequest: RequestState = { phase: 'error', lastSuccessAt: request.lastSuccessAt, error: action.message };
      return action.source === 'events'
        ? { ...state, eventsRequest: nextRequest }
        : { ...state, healthRequest: nextRequest };
    }
    case 'rootPending':
      return {
        ...state,
        pendingRoot: {
          ...state.pendingRoot,
          [action.device]: {
            desired: action.active,
            requestId: action.requestId,
            baselineCursor: action.baselineCursor,
            baselineEpoch: action.baselineEpoch,
            phase: 'writing',
          },
        },
        announcement: `Sending ROOT ${action.active ? 'ON' : 'OFF'} to Device ${action.device}.`,
      };
    case 'rootAccepted': {
      const pending = state.pendingRoot[action.device];
      if (!pending || pending.requestId !== action.requestId) return state;
      return {
        ...state,
        pendingRoot: { ...state.pendingRoot, [action.device]: { ...pending, phase: 'confirming' } },
        announcement: `Device ${action.device} accepted serial write for ROOT ${action.command.toUpperCase()}; waiting for authoritative confirmation.`,
      };
    }
    case 'rootFailed': {
      const pendingRoot = clearPendingRoot(state.pendingRoot, action.device, action.requestId);
      if (pendingRoot === state.pendingRoot) return state;
      return {
        ...state,
        pendingRoot,
        announcement: `Device ${action.device} root command failed: ${action.message}`,
      };
    }
    case 'rootConfirmationTimedOut': {
      const pending = state.pendingRoot[action.device];
      if (!pending || pending.requestId !== action.requestId) return state;
      return {
        ...state,
        pendingRoot: clearPendingRoot(state.pendingRoot, action.device, action.requestId),
        announcement: `Device ${action.device} did not publish the requested ROOT ${pending.desired ? 'ON' : 'OFF'} state in time; retry this device.`,
      };
    }
  }
}

export interface BridgeStatus {
  kind: 'loading' | 'online' | 'reconnecting' | 'stale' | 'offline';
  message: string;
}

export function bridgeStatus(state: DashboardState, now: number): BridgeStatus {
  const lastSuccess = Math.max(state.eventsRequest.lastSuccessAt ?? 0, state.healthRequest.lastSuccessAt ?? 0);
  const hasError = state.eventsRequest.phase === 'error' || state.healthRequest.phase === 'error';
  if (lastSuccess === 0) {
    return hasError
      ? { kind: 'offline', message: 'Bridge offline — retrying' }
      : { kind: 'loading', message: 'Connecting to bridge' };
  }
  if (now - lastSuccess >= STALE_AFTER_MS) {
    return { kind: 'stale', message: 'Bridge data is stale' };
  }
  if (hasError) return { kind: 'reconnecting', message: 'Bridge reconnecting — showing last known data' };
  return { kind: 'online', message: 'Bridge receiving records' };
}
