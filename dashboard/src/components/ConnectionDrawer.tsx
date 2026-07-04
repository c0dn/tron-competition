import { useState, useEffect } from 'react';
import type { ConnStatus } from '../lib/useMqtt';

interface Props {
  open: boolean;
  url: string;
  status: ConnStatus;
  onClose: () => void;
  onConnect: (url: string) => void;
}

export function ConnectionDrawer({ open, url, status, onClose, onConnect }: Props) {
  const [draft, setDraft] = useState(url);
  useEffect(() => setDraft(url), [url]);

  return (
    <>
      <div className={`scrim${open ? ' show' : ''}`} onClick={onClose} />
      <aside className={`drawer${open ? ' open' : ''}`} aria-hidden={!open}>
        <div className="drawer-head">
          <h3>Connection</h3>
          <button className="icon-btn" onClick={onClose} aria-label="close">✕</button>
        </div>

        <label className="field">
          <span>Broker WebSocket URL</span>
          <input
            value={draft}
            onChange={(e) => setDraft(e.target.value)}
            placeholder="ws://host:9001"
            spellCheck={false}
          />
        </label>

        <div className={`status-line ${status}`}>
          <span className={`dot ${status === 'online' ? 'up' : status === 'connecting' ? 'warn' : 'down'}`} />
          {status}
        </div>

        <button className="primary" onClick={() => onConnect(draft.trim())}>
          {status === 'online' ? 'Reconnect' : 'Connect'}
        </button>

        <p className="hint">
          The broker must expose its MQTT-over-WebSocket listener (port 9001 in
          the bundled <code>infra/mosquitto.conf</code>). Subscribes to
          <code>mind/ingest/#</code> and <code>mind/node/+/lwt</code>.
        </p>
      </aside>
    </>
  );
}
