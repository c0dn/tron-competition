import type { BridgeStatus } from '../state/store';

interface HeaderProps {
  status: BridgeStatus;
}

export function Header({ status }: HeaderProps) {
  return (
    <header className="app-header">
      <div>
        <p className="eyebrow">MIND · competition operations</p>
        <h1>KISS operations dashboard</h1>
      </div>
      <p className={`bridge-status ${status.kind}`} role="status" aria-live="polite" aria-label={`Bridge: ${status.message}`}>
        <strong>Bridge:</strong>
        <span>{status.message}</span>
      </p>
    </header>
  );
}
