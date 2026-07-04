/*
 * store.ts — dashboard state + reducer.
 *
 * Folds the stream of decoded uplink records (plus retained LWT presence) into
 * three views: wearables (with a rolling SVM history for sparklines), ESP32-C3
 * nodes, and an incident log that can be filtered and acknowledged.
 */
import {
  EventRecord,
  StatusRecord,
  Severity,
  eventTypeInfo,
} from '../lib/uplink';

export const SVM_HISTORY = 40; // sparkline sample window
export const FEED_MAX = 100;

export interface Wearable extends EventRecord {
  lastSeen: number;
  svmHistory: number[];
}

export interface MindNode extends Partial<StatusRecord> {
  nodeId: number;
  lastSeen?: number;
  rootPresence?: 'online' | 'offline';
}

export interface Incident {
  id: number;
  ts: number;
  deviceId: number;
  nodeId: number;
  name: string;
  severity: Severity;
  confidence: number;
  rssi: number;
  acked: boolean;
}

export interface State {
  wearables: Record<number, Wearable>;
  nodes: Record<number, MindNode>;
  incidents: Incident[];
  nextIncidentId: number;
}

export const initialState: State = {
  wearables: {},
  nodes: {},
  incidents: [],
  nextIncidentId: 1,
};

export type Action =
  | { type: 'event'; rec: EventRecord; ts: number }
  | { type: 'status'; rec: StatusRecord; ts: number }
  | { type: 'lwt'; nodeId: number; presence: 'online' | 'offline' }
  | { type: 'ack'; id: number }
  | { type: 'ackAll' }
  | { type: 'clearAcked' };

export function reducer(state: State, action: Action): State {
  switch (action.type) {
    case 'event': {
      const { rec, ts } = action;
      const prev = state.wearables[rec.deviceId];
      const history = [...(prev?.svmHistory ?? []), rec.accelSvm].slice(-SVM_HISTORY);
      const wearable: Wearable = { ...rec, lastSeen: ts, svmHistory: history };

      const info = eventTypeInfo(rec.eventType);
      let incidents = state.incidents;
      let nextIncidentId = state.nextIncidentId;
      if (info.severity >= 2) {
        const incident: Incident = {
          id: nextIncidentId,
          ts,
          deviceId: rec.deviceId,
          nodeId: rec.nodeId,
          name: info.name,
          severity: info.severity,
          confidence: rec.confidence,
          rssi: rec.rssi,
          acked: false,
        };
        incidents = [incident, ...state.incidents].slice(0, FEED_MAX);
        nextIncidentId += 1;
      }

      return {
        ...state,
        wearables: { ...state.wearables, [rec.deviceId]: wearable },
        incidents,
        nextIncidentId,
      };
    }

    case 'status': {
      const { rec, ts } = action;
      const prev = state.nodes[rec.nodeId] ?? { nodeId: rec.nodeId };
      const node: MindNode = { ...prev, ...rec, lastSeen: ts };
      return { ...state, nodes: { ...state.nodes, [rec.nodeId]: node } };
    }

    case 'lwt': {
      const prev = state.nodes[action.nodeId] ?? { nodeId: action.nodeId };
      const node: MindNode = { ...prev, rootPresence: action.presence };
      return { ...state, nodes: { ...state.nodes, [action.nodeId]: node } };
    }

    case 'ack':
      return {
        ...state,
        incidents: state.incidents.map((i) =>
          i.id === action.id ? { ...i, acked: true } : i,
        ),
      };

    case 'ackAll':
      return { ...state, incidents: state.incidents.map((i) => ({ ...i, acked: true })) };

    case 'clearAcked':
      return { ...state, incidents: state.incidents.filter((i) => !i.acked) };

    default:
      return state;
  }
}

/* Derived helpers ---------------------------------------------------- */
export const STALE_MS = 15000;

export function isWearableStale(w: Wearable, now: number): boolean {
  return now - w.lastSeen >= STALE_MS;
}

export function isNodeOnline(n: MindNode, now: number): boolean {
  if (n.isRoot && n.rootPresence === 'offline') return false;
  return n.lastSeen != null && now - n.lastSeen < STALE_MS;
}
