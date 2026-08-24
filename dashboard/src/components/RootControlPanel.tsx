import type { HealthDevice, RootRecord } from '../lib/api';
import { authoritativeRootFor, gatewayDevice } from '../state/devices';
import type { PendingRootRequest } from '../state/store';

interface RootControlPanelProps {
  devices: HealthDevice[];
  healthEpoch: number | null;
  currentEpoch: number;
  healthCurrentCursor: number | null;
  rootRecords: Record<number, RootRecord>;
  pendingRoot: Record<number, PendingRootRequest | undefined>;
  rootErrors?: Record<number, string | undefined>;
  announcement: string;
  healthError?: string;
  healthAuthoritative?: boolean;
  onSetRoot: (device: number, active: boolean) => void;
}

function gatewayErrorMessage(error: string | undefined, fallback: string): string | undefined {
  if (!error) return undefined;
  if (/(?:\/api\/root|root(?:\b|_))/i.test(error)) return fallback;
  return error;
}

export function RootControlPanel({
  devices,
  healthEpoch,
  currentEpoch,
  healthCurrentCursor,
  rootRecords,
  pendingRoot,
  rootErrors,
  announcement,
  healthError,
  healthAuthoritative = true,
  onSetRoot,
}: RootControlPanelProps) {
  const gateway = gatewayDevice(devices);
  const root = healthAuthoritative && gateway?.connected
    ? authoritativeRootFor(gateway, healthEpoch, currentEpoch, healthCurrentCursor, rootRecords[0])
    : null;
  const pending = pendingRoot[0];
  const error = gatewayErrorMessage(rootErrors?.[0], 'Gateway command could not be completed. Retry Gateway.');
  const visibleHealthError = gatewayErrorMessage(healthError, 'Gateway health is unavailable. Retrying.');
  const active = root?.role === 'root';
  const unknown = root === null;
  const disconnected = !gateway?.connected;
  const disabled = !healthAuthoritative || !gateway || disconnected || unknown || pending !== undefined;
  const state = !gateway
    ? 'Gateway unavailable'
    : !healthAuthoritative
      ? 'Refreshing'
      : disconnected
      ? 'Disconnected'
      : pending?.phase === 'writing'
        ? 'Writing'
        : pending?.phase === 'confirming'
          ? 'Confirming'
          : error
            ? 'Error'
            : unknown
              ? 'Unknown'
              : active
                ? 'On'
                : 'Off';
  const actionLabel = unknown || !gateway
    ? 'Gateway status unknown'
    : `Turn Gateway ${active ? 'off' : 'on'}`;
  const commandStatus = announcement
    || (error
      ? 'Gateway command failed.'
      : !gateway
        ? 'Gateway unavailable.'
        : !healthAuthoritative
          ? 'Gateway health is refreshing.'
          : disconnected
            ? 'Gateway disconnected.'
            : unknown
              ? 'Waiting for authoritative Gateway status.'
              : `Gateway is ${active ? 'on' : 'off'}.`);

  return (
    <section className="panel gateway-panel" aria-labelledby="gateway-heading">
      <div className="section-heading">
        <div>
          <p className="eyebrow">Gateway</p>
          <h2 id="gateway-heading">Gateway control</h2>
        </div>
        <p className="command-status" role="status" aria-live="polite" aria-label="Gateway command status">
          {commandStatus}
        </p>
      </div>
      {visibleHealthError && <p className="notice error" role="alert">Gateway health unavailable: {visibleHealthError}</p>}
      <div className="gateway-control-row">
        <div>
          <strong>Gateway</strong>
          <span>{root ? `AdvA ${root.local}` : 'Waiting for Gateway status'}</span>
        </div>
        <span className={`status-chip ${state === 'On' ? 'good' : state === 'Error' || state === 'Disconnected' ? 'danger' : state === 'Off' ? 'info' : 'warning'}`}>
          {state}
        </span>
        <div className="root-switch-row">
          <button
            type="button"
            role={unknown ? 'checkbox' : 'switch'}
            aria-checked={unknown ? 'mixed' : active}
            aria-label={actionLabel}
            aria-describedby="gateway-control-detail"
            disabled={disabled}
            className={`root-switch${active ? ' checked' : ''}${unknown ? ' unknown' : ''}`}
            onClick={() => {
              if (gateway && root) onSetRoot(0, !active);
            }}
          >
            <span aria-hidden="true" className="switch-track"><span className="switch-thumb" /></span>
            <span>{pending ? state : active ? 'Gateway on' : unknown ? 'Status unknown' : 'Gateway off'}</span>
          </button>
          <span id="gateway-control-detail" className="root-switch-detail">
            {error
              ? error
              : pending
                ? `${pending.phase === 'writing' ? 'Writing command.' : 'Waiting for confirmation.'}`
                : disconnected
                  ? 'Reconnect the Gateway to change state.'
                  : !healthAuthoritative
                    ? 'Waiting for current Gateway health before changing state.'
                  : unknown
                    ? 'Waiting for authoritative Gateway status.'
                    : `Activate to turn Gateway ${active ? 'off' : 'on'}.`}
          </span>
        </div>
      </div>
    </section>
  );
}
