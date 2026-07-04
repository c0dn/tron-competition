import type { ConnStatus } from '../lib/useMqtt';

interface Props {
  status: ConnStatus;
  url: string;
  onOpenSettings: () => void;
}

export function Header({ status, url, onOpenSettings }: Props) {
  return (
    <header>
      <div className="brand">
        <h1>MIND · Live Monitor</h1>
        <div className="sub">Multimodal Incident Detection — TRON 2026</div>
      </div>
      <div className="spacer" />
      <span className={`conn ${status}`} title={url}>
        <span className={`dot ${status === 'online' ? 'up' : status === 'connecting' ? 'warn' : 'down'}`} />
        {status}
      </span>
      <button className="icon-btn" onClick={onOpenSettings} aria-label="connection settings">⚙</button>
    </header>
  );
}
