/*
 * binaryReader.js — JS mirror of shared/uplink_schema.h.
 *
 * Decodes the packed little-endian records the ESP32-C3 root republishes on
 * mind/ingest/*. Keep the byte offsets here in lock-step with uplink_schema.h;
 * a size mismatch throws so drift is caught loudly rather than silently
 * mis-decoded.
 */

export const UPLINK_VERSION = 1;

export const REC_EVENT = 1;
export const REC_STATUS = 2;

export const EVENT_EVENT_SIZE = 13;
export const EVENT_STATUS_SIZE = 15;

/* enum mind_event_type (schema.h) -> label + severity (0..3). */
export const EVENT_TYPES = {
  0: { name: 'heartbeat',         severity: 0 },
  1: { name: 'motion',            severity: 1 },
  2: { name: 'possible fall',     severity: 2 },
  3: { name: 'CONFIRMED FALL',    severity: 3 },
  4: { name: 'possible distress', severity: 2 },
  5: { name: 'FALL + SHOUT',      severity: 3 },
};

export function eventTypeInfo(t) {
  return EVENT_TYPES[t] || { name: `unknown(${t})`, severity: 1 };
}

/* Accept Uint8Array | ArrayBuffer | Buffer(node-style) and return a DataView
 * over exactly its bytes (respecting byteOffset for subarray views). */
function toView(payload) {
  if (payload instanceof DataView) return payload;
  if (payload instanceof ArrayBuffer) return new DataView(payload);
  // Uint8Array or Buffer
  return new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
}

/*
 * Decode one binary record. Returns:
 *   { kind: 'event',  ...fields }  or
 *   { kind: 'status', ...fields }
 * Throws on unknown rec_type or wrong length.
 */
export function decodeRecord(payload) {
  const dv = toView(payload);
  if (dv.byteLength < 2) throw new Error('record too short');

  const version = dv.getUint8(0);
  const recType = dv.getUint8(1);

  if (recType === REC_EVENT) {
    if (dv.byteLength !== EVENT_EVENT_SIZE) {
      throw new Error(`event record is ${dv.byteLength}B, expected ${EVENT_EVENT_SIZE}`);
    }
    return {
      kind: 'event',
      version,
      nodeId:     dv.getUint8(2),
      deviceId:   dv.getUint8(3),
      rssi:       dv.getInt8(4),
      eventType:  dv.getUint8(5),
      confidence: dv.getUint8(6),
      accelSvm:   dv.getUint16(7, true),
      micLevel:   dv.getUint8(9),
      seq:        dv.getUint8(10),
      ageMs:      dv.getUint16(11, true),
    };
  }

  if (recType === REC_STATUS) {
    if (dv.byteLength !== EVENT_STATUS_SIZE) {
      throw new Error(`status record is ${dv.byteLength}B, expected ${EVENT_STATUS_SIZE}`);
    }
    return {
      kind: 'status',
      version,
      nodeId:      dv.getUint8(2),
      isRoot:      dv.getUint8(3) === 1,
      meshLevel:   dv.getUint8(4),
      parentRssi:  dv.getInt8(5),
      uptimeS:     dv.getUint32(6, true),
      freeHeap:    dv.getUint32(10, true),
      childCount:  dv.getUint8(14),
    };
  }

  throw new Error(`unknown rec_type ${recType}`);
}
