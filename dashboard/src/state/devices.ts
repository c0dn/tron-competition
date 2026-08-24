import type { GttEntry, GttSnapshot, HealthDevice, HealthRoot, RootRecord, RootStatus } from '../lib/api';

export interface GatewayTopology {
  gateway: HealthDevice;
  root: HealthRoot | RootRecord | null;
  gtt: GttSnapshot;
  entries: GttEntry[];
}

export type GatewayTopologyIssue =
  | 'gateway_missing'
  | 'gateway_duplicate'
  | 'gateway_disconnected'
  | 'health_unavailable'
  | 'gtt_missing'
  | 'gtt_self_missing'
  | 'gtt_self_duplicate'
  | 'gtt_root_mismatch';

export type GatewayTopologyResult =
  | { available: true; topology: GatewayTopology }
  | { available: false; issue: GatewayTopologyIssue; gateway: HealthDevice | null };

export function physicalDevices(devices: HealthDevice[]): HealthDevice[] {
  return devices
    .filter((device) => device.device === device.owner_device)
    .sort((left, right) => left.device - right.device);
}

export function authoritativeRootFor(
  device: HealthDevice,
  healthEpoch: number | null,
  currentEpoch: number,
  healthCurrentCursor: number | null,
  recent?: RootRecord,
): (HealthRoot | RootRecord) | null {
  if (!device.connected) return null;
  if (healthEpoch !== currentEpoch) return recent ?? null;
  if (device.root !== null) {
    return !recent || device.root.cursor >= recent.cursor ? device.root : recent;
  }
  if (!recent || healthCurrentCursor === null || recent.cursor > healthCurrentCursor) return recent ?? null;
  return null;
}

export function gatewayDevice(devices: HealthDevice[]): HealthDevice | null {
  const owners = physicalDevices(devices);
  if (owners.length !== 1 || owners[0]?.device !== 0 || owners[0]?.owner_device !== 0) return null;
  return owners[0];
}

export function gatewayTopology(
  devices: HealthDevice[],
  healthEpoch: number | null,
  currentEpoch: number,
  healthCurrentCursor: number | null,
  rootRecords: Record<number, RootRecord>,
  healthAuthoritative = true,
): GatewayTopologyResult {
  const owners = physicalDevices(devices);
  if (owners.length === 0 || owners.every((device) => device.device !== 0)) {
    return { available: false, issue: 'gateway_missing', gateway: null };
  }
  if (owners.length !== 1 || owners[0].device !== 0) {
    return { available: false, issue: 'gateway_duplicate', gateway: owners.find((device) => device.device === 0) ?? null };
  }
  const gateway = owners[0];
  if (!healthAuthoritative) return { available: false, issue: 'health_unavailable', gateway };
  if (!gateway.connected) return { available: false, issue: 'gateway_disconnected', gateway };
  const root = authoritativeRootFor(gateway, healthEpoch, currentEpoch, healthCurrentCursor, rootRecords[0]);
  const gtt = healthEpoch === currentEpoch ? gateway.gtt : null;
  if (!gtt) return { available: false, issue: 'gtt_missing', gateway };
  const selfEntries = gtt.entries.filter((entry) => entry.adva === gtt.local);
  if (selfEntries.length === 0) return { available: false, issue: 'gtt_self_missing', gateway };
  if (selfEntries.length !== 1) return { available: false, issue: 'gtt_self_duplicate', gateway };
  if (root && root.local !== gtt.local) return { available: false, issue: 'gtt_root_mismatch', gateway };
  return { available: true, topology: { gateway, root, gtt, entries: gtt.entries } };
}

export function rootActive(
  device: HealthDevice,
  healthEpoch: number | null,
  currentEpoch: number,
  healthCurrentCursor: number | null,
  rootRecords: Record<number, RootRecord>,
): boolean {
  return authoritativeRootFor(device, healthEpoch, currentEpoch, healthCurrentCursor, rootRecords[device.device])?.role === 'root';
}

export function rootRequestBaseline(
  eventCursor: number,
  deviceIndex: number,
  healthDevice: HealthDevice | undefined,
  healthEpoch: number | null,
  currentEpoch: number,
  healthCurrentCursor: number | null,
  rootRecords: Record<number, RootRecord>,
): number {
  const authority = healthDevice
    ? authoritativeRootFor(healthDevice, healthEpoch, currentEpoch, healthCurrentCursor, rootRecords[deviceIndex])
    : rootRecords[deviceIndex] ?? null;
  return Math.max(eventCursor, authority?.cursor ?? 0);
}

export function gatewayEntryLabel(entry: GttEntry, local: string): string {
  return entry.adva === local ? 'Gateway' : entry.adva;
}

export function nodeIdentity(_device: HealthDevice, root: RootStatus | null): { primary: string; secondary: string } {
  return root
    ? { primary: 'Gateway', secondary: `AdvA ${root.local}` }
    : { primary: 'Gateway', secondary: 'Awaiting Gateway status' };
}
