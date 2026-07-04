/*
 * uplink.ts — typed mirror of shared/uplink_schema.h.
 *
 * Decodes the packed little-endian records the ESP32-C3 root republishes on
 * mind/ingest/*. Byte offsets must stay in lock-step with the C header; a size
 * mismatch throws so drift is caught loudly rather than mis-decoded.
 */

export const UPLINK_VERSION = 1;
export const REC_EVENT = 1;
export const REC_STATUS = 2;
export const EVENT_SIZE = 13;
export const STATUS_SIZE = 15;

export type Severity = 0 | 1 | 2 | 3;

export interface EventRecord {
  kind: 'event';
  version: number;
  nodeId: number;
  deviceId: number;
  rssi: number;
  eventType: number;
  confidence: number;
  accelSvm: number;
  micLevel: number;
  seq: number;
  ageMs: number;
}

export interface StatusRecord {
  kind: 'status';
  version: number;
  nodeId: number;
  isRoot: boolean;
  meshLevel: number;
  parentRssi: number;
  uptimeS: number;
  freeHeap: number;
  childCount: number;
}

export type UplinkRecord = EventRecord | StatusRecord;

export interface EventTypeInfo {
  name: string;
  severity: Severity;
}

/* enum mind_event_type (schema.h) -> label + severity. */
const EVENT_TYPES: Record<number, EventTypeInfo> = {
  0: { name: 'heartbeat', severity: 0 },
  1: { name: 'motion', severity: 1 },
  2: { name: 'possible fall', severity: 2 },
  3: { name: 'CONFIRMED FALL', severity: 3 },
  4: { name: 'possible distress', severity: 2 },
  5: { name: 'FALL + SHOUT', severity: 3 },
};

export function eventTypeInfo(t: number): EventTypeInfo {
  return EVENT_TYPES[t] ?? { name: `unknown(${t})`, severity: 1 };
}

/* Accept the various shapes MQTT.js hands us and view exactly its bytes. */
function toView(payload: Uint8Array | ArrayBuffer | DataView): DataView {
  if (payload instanceof DataView) return payload;
  if (payload instanceof ArrayBuffer) return new DataView(payload);
  return new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
}

export function decodeRecord(payload: Uint8Array | ArrayBuffer | DataView): UplinkRecord {
  const dv = toView(payload);
  if (dv.byteLength < 2) throw new Error('record too short');

  const version = dv.getUint8(0);
  const recType = dv.getUint8(1);

  if (recType === REC_EVENT) {
    if (dv.byteLength !== EVENT_SIZE) {
      throw new Error(`event record is ${dv.byteLength}B, expected ${EVENT_SIZE}`);
    }
    return {
      kind: 'event',
      version,
      nodeId: dv.getUint8(2),
      deviceId: dv.getUint8(3),
      rssi: dv.getInt8(4),
      eventType: dv.getUint8(5),
      confidence: dv.getUint8(6),
      accelSvm: dv.getUint16(7, true),
      micLevel: dv.getUint8(9),
      seq: dv.getUint8(10),
      ageMs: dv.getUint16(11, true),
    };
  }

  if (recType === REC_STATUS) {
    if (dv.byteLength !== STATUS_SIZE) {
      throw new Error(`status record is ${dv.byteLength}B, expected ${STATUS_SIZE}`);
    }
    return {
      kind: 'status',
      version,
      nodeId: dv.getUint8(2),
      isRoot: dv.getUint8(3) === 1,
      meshLevel: dv.getUint8(4),
      parentRssi: dv.getInt8(5),
      uptimeS: dv.getUint32(6, true),
      freeHeap: dv.getUint32(10, true),
      childCount: dv.getUint8(14),
    };
  }

  throw new Error(`unknown rec_type ${recType}`);
}
