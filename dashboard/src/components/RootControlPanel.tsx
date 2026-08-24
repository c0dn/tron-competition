import type { CommandRecord, HealthDevice, RootRecord } from '../lib/api';
import { authoritativeRootFor, nodeIdentity, physicalDevices } from '../state/devices';
import type { PendingRootRequest } from '../state/store';

interface RootControlPanelProps {
  devices: HealthDevice[];
  healthEpoch: number | null;
  currentEpoch: number;
  healthCurrentCursor: number | null;
  rootRecords: Record<number, RootRecord>;
  commandRecords: Record<number, CommandRecord>;
  pendingRoot: Record<number, PendingRootRequest | undefined>;
  announcement: string;
  healthError?: string;
  onSetRoot: (device: number, active: boolean) => void;
}

function commandLabel(command: CommandRecord): string {
  return `Last firmware command: ${command.command.toUpperCase()} (${command.status})`;
}

export function RootControlPanel({
  devices,
  healthEpoch,
  currentEpoch,
  healthCurrentCursor,
  rootRecords,
  commandRecords,
  pendingRoot,
  announcement,
  healthError,
  onSetRoot,
}: RootControlPanelProps) {
  const owners = physicalDevices(devices);
  return (
    <section className="panel root-panel" aria-labelledby="bridge-controls-heading">
      <div className="section-heading">
        <div>
          <p className="eyebrow">Bridge and serial health</p>
          <h2 id="bridge-controls-heading">Root devices</h2>
        </div>
        <p className="command-status" role="status" aria-live="polite" aria-label="Root command status">
          {announcement || 'Runtime role is confirmed by the bridge after each serial command.'}
        </p>
      </div>
      {healthError && <p className="notice error" role="alert">Health request failed: {healthError} Retrying while preserving the last known devices.</p>}
      {owners.length === 0 ? (
        <p className="empty">No configured serial devices. Start the bridge with one or more serial paths.</p>
      ) : (
        <div className="table-wrap">
          <table className="device-table">
            <caption>Connected physical nodes and their authoritative runtime roles</caption>
            <thead>
              <tr>
                <th scope="col">Node</th>
                <th scope="col">Serial</th>
                <th scope="col">Runtime</th>
                <th scope="col">Root control</th>
              </tr>
            </thead>
            <tbody>
              {owners.map((device) => {
                const root = authoritativeRootFor(device, healthEpoch, currentEpoch, healthCurrentCursor, rootRecords[device.device]);
                const identity = nodeIdentity(device, root);
                const active = root?.role === 'root';
                const pending = pendingRoot[device.device];
                const unknown = root === null;
                const disabled = !device.connected || pending !== undefined || unknown;
                const switchLabel = unknown
                  ? `Root role unknown for Device ${device.device}`
                  : `${active ? 'Turn root off' : 'Turn root on'} for ${identity.primary}`;
                const command = commandRecords[device.device];
                const rootCommand = command?.command === 'gtt' ? undefined : command;
                const serialIssues = device.reconnects + device.parse_errors + device.overlong_lines;
                return (
                  <tr key={device.device}>
                    <th scope="row">
                      <strong>{identity.primary}</strong>
                      <span>{identity.secondary}</span>
                    </th>
                    <td data-label="Serial">
                      <strong>{device.connected ? 'Connected' : 'Offline'}</strong>
                      {serialIssues > 0 && <span>{device.reconnects} reconnects · {device.parse_errors} parse errors · {device.overlong_lines} overlong</span>}
                    </td>
                    <td data-label="Runtime">
                      <span className={`status-chip ${!device.connected ? 'danger' : pending ? 'warning' : unknown ? 'muted' : active ? 'good' : 'info'}`}>
                        {!device.connected ? 'Serial offline' : pending ? `ROOT ${pending.desired ? 'ON' : 'OFF'} pending` : unknown ? 'Role unknown' : active ? 'Runtime root' : 'Runtime leaf'}
                      </span>
                      <span>{root ? `${root.roots} active root${root.roots === 1 ? '' : 's'} · ${root.pending} pending` : 'Awaiting ROOT STATUS'}</span>
                      {rootCommand && <small>{commandLabel(rootCommand)}</small>}
                    </td>
                    <td data-label="Root control">
                      <div className="root-switch-row">
                        <button
                          type="button"
                          role={unknown ? 'checkbox' : 'switch'}
                          aria-checked={unknown ? 'mixed' : active}
                          aria-label={switchLabel}
                          aria-describedby={`root-detail-${device.device}`}
                          disabled={disabled}
                          className={`root-switch${active ? ' checked' : ''}${unknown ? ' unknown' : ''}`}
                          onClick={() => {
                            if (root) onSetRoot(device.device, !active);
                          }}
                        >
                          <span aria-hidden="true" className="switch-track"><span className="switch-thumb" /></span>
                          <span>{unknown ? 'Role unknown' : active ? 'Root on' : 'Root off'}</span>
                        </button>
                        <span id={`root-detail-${device.device}`} className="root-switch-detail">
                          {pending
                            ? `Requested ROOT ${pending.desired ? 'ON' : 'OFF'}; ${pending.phase === 'writing' ? 'writing serial command' : 'awaiting bridge confirmation'}.`
                            : unknown
                              ? 'Disabled while awaiting bridge status.'
                              : device.connected
                                ? `Click to request ROOT ${active ? 'OFF' : 'ON'}.`
                                : 'Reconnect the serial device before changing its role.'}
                        </span>
                      </div>
                    </td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        </div>
      )}
    </section>
  );
}
