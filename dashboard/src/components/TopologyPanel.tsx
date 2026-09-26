import type { HealthDevice, RootRecord } from '../lib/api';
import { gatewayTopology } from '../state/devices';

interface TopologyPanelProps {
  devices: HealthDevice[];
  healthEpoch: number | null;
  currentEpoch: number;
  healthCurrentCursor: number | null;
  rootRecords: Record<number, RootRecord>;
  now: number;
}

/**
 * Kept as a narrow compatibility seam for callers that previously mounted a
 * topology card. The production surface is FloorplanPanel, which owns the
 * Gateway GTT roster and map so there is no alternate source selector.
 */
export function TopologyPanel({ devices, healthEpoch, currentEpoch, healthCurrentCursor, rootRecords }: TopologyPanelProps) {
  const topology = gatewayTopology(devices, healthEpoch, currentEpoch, healthCurrentCursor, rootRecords);
  return (
    <section className="panel topology-panel" aria-labelledby="gateway-roster-heading">
      <div className="section-heading"><h2 id="gateway-roster-heading">Gateway roster</h2></div>
      {topology.available
        ? <p className="empty">{topology.topology.entries.length} Gateway GTT entries are available on the floorplan.</p>
        : <p className="empty">Gateway topology unavailable.</p>}
    </section>
  );
}
