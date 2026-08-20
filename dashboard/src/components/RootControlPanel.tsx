import type { CommandRecord, HealthDevice, RootRecord } from '../lib/api';

interface RootControlPanelProps {
  devices: HealthDevice[];
  healthCurrentCursor?: number;
  healthEpoch: number | null;
  currentEpoch: number;
  rootRecords: Record<number, RootRecord>;
  commandRecords: Record<number, CommandRecord>;
  pendingRoot: Record<number, boolean>;
  announcement: string;
  healthError?: string;
  onSetRoot: (device: number, active: boolean) => void;
}

function rootFor(
  device: HealthDevice,
  healthCurrentCursor: number | undefined,
  healthEpoch: number | null,
  currentEpoch: number,
  recent?: RootRecord,
) {
  if (healthEpoch !== currentEpoch) return recent;
  if (!recent || (healthCurrentCursor !== undefined && healthCurrentCursor >= recent.cursor)) return device.root;
  return recent;
}

export function RootControlPanel({
  devices,
  healthCurrentCursor,
  healthEpoch,
  currentEpoch,
  rootRecords,
  commandRecords,
  pendingRoot,
  announcement,
  healthError,
  onSetRoot,
}: RootControlPanelProps) {
  return (
    <section className="panel" aria-labelledby="bridge-controls-heading">
      <div className="section-heading">
        <div>
          <p className="eyebrow">Bridge and serial health</p>
          <h2 id="bridge-controls-heading">Root controls</h2>
        </div>
        <p className="command-status" role="status" aria-live="polite" aria-label="Root command status">
          {announcement || 'Choose a device to request its runtime root role.'}
        </p>
      </div>
      {healthError && <p className="notice error" role="alert">Health request failed: {healthError} Retrying while preserving the last known devices.</p>}
      {devices.length === 0 ? (
        <p className="empty">No configured serial devices. Start the bridge with one or more serial paths.</p>
      ) : (
        <div className="table-wrap">
          <table className="device-table">
            <caption>Configured serial devices and runtime root requests</caption>
            <thead>
              <tr>
                <th scope="col">Device</th>
                <th scope="col">Serial health</th>
                <th scope="col">Latest root state</th>
                <th scope="col">Controls</th>
              </tr>
            </thead>
            <tbody>
              {devices.map((device) => {
                const root = rootFor(device, healthCurrentCursor, healthEpoch, currentEpoch, rootRecords[device.device]);
                const command = commandRecords[device.device];
                const pending = pendingRoot[device.device] === true;
                return (
                  <tr key={device.device}>
                    <th scope="row">
                      <strong>Device {device.device}</strong>
                      <code>{device.path}</code>
                    </th>
                    <td data-label="Serial health">
                      <strong>{device.connected ? 'Serial connected' : 'Serial offline'}</strong>
                      <span>Reconnects {device.reconnects}; parse errors {device.parse_errors}; overlong lines {device.overlong_lines}</span>
                    </td>
                    <td data-label="Latest root state">
                      {root ? (
                        <>
                          <strong>{root.role === 'root' ? 'Runtime root' : 'Leaf'}</strong>
                          <span>Node {root.node}; {root.roots} active roots; pending {root.pending}</span>
                        </>
                      ) : (
                        <span>No root record yet</span>
                      )}
                      {command && <small>Last UART command: {command.command} ({command.status})</small>}
                    </td>
                    <td data-label="Root controls">
                      <div className="root-actions" aria-label={`Root controls for Device ${device.device}`}>
                        <button type="button" aria-label={`Request ROOT ON for Device ${device.device}`} disabled={pending} onClick={() => onSetRoot(device.device, true)}>
                          {pending ? 'Sending request…' : 'Root ON'}
                        </button>
                        <button type="button" aria-label={`Request ROOT OFF for Device ${device.device}`} className="secondary" disabled={pending} onClick={() => onSetRoot(device.device, false)}>
                          Root OFF
                        </button>
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
