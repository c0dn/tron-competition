/*
 * useMqtt — connect to the broker over WebSocket, subscribe, and dispatch
 * decoded records into the store. Re-runs (reconnects) when `url` or
 * `generation` change; `generation` lets the UI force a reconnect to the same
 * URL (the Connect button).
 */
import { useEffect, useRef, useState } from 'react';
import mqtt, { MqttClient } from 'mqtt';
import { decodeRecord } from './uplink';
import type { Action } from '../state/store';

export type ConnStatus = 'offline' | 'connecting' | 'online';

const decoder = new TextDecoder();

export function useMqtt(
  url: string,
  generation: number,
  dispatch: (a: Action) => void,
): ConnStatus {
  const [status, setStatus] = useState<ConnStatus>('offline');
  const clientRef = useRef<MqttClient | null>(null);

  useEffect(() => {
    if (!url) return;
    setStatus('connecting');
    const client = mqtt.connect(url, { reconnectPeriod: 2000, connectTimeout: 8000 });
    clientRef.current = client;

    client.on('connect', () => {
      setStatus('online');
      client.subscribe('mind/ingest/#');
      client.subscribe('mind/node/+/lwt');
    });
    client.on('reconnect', () => setStatus('connecting'));
    client.on('close', () => setStatus('offline'));
    client.on('error', () => setStatus('offline'));

    client.on('message', (topic, payload: Uint8Array) => {
      if (topic.startsWith('mind/ingest/')) {
        try {
          const rec = decodeRecord(payload);
          const ts = Date.now();
          if (rec.kind === 'event') dispatch({ type: 'event', rec, ts });
          else dispatch({ type: 'status', rec, ts });
        } catch (e) {
          console.warn('decode failed:', (e as Error).message);
        }
        return;
      }
      const m = topic.match(/^mind\/node\/(\d+)\/lwt$/);
      if (m) {
        const presence = decoder.decode(payload) === 'offline' ? 'offline' : 'online';
        dispatch({ type: 'lwt', nodeId: Number(m[1]), presence });
      }
    });

    return () => {
      client.end(true);
      clientRef.current = null;
    };
  }, [url, generation, dispatch]);

  return status;
}
