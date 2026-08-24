import type { HealthDevice, HealthRoot, RootRecord, RootStatus } from '../lib/api';

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
  if (healthEpoch !== currentEpoch) return recent ?? null;
  if (device.root !== null) {
    return !recent || device.root.cursor >= recent.cursor ? device.root : recent;
  }
  if (!recent || healthCurrentCursor === null || recent.cursor > healthCurrentCursor) return recent ?? null;
  return null;
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

export function eligibleTopologySources(
  devices: HealthDevice[],
  healthEpoch: number | null,
  currentEpoch: number,
  healthCurrentCursor: number | null,
  rootRecords: Record<number, RootRecord>,
): HealthDevice[] {
  const connectedOwners = physicalDevices(devices).filter((device) => device.connected);
  const activeRoots = connectedOwners.filter((device) => rootActive(
    device,
    healthEpoch,
    currentEpoch,
    healthCurrentCursor,
    rootRecords,
  ));
  return activeRoots.length > 0 ? activeRoots : connectedOwners;
}

export function nodeIdentity(device: HealthDevice, root: RootStatus | null): { primary: string; secondary: string } {
  if (!root) {
    return { primary: `Device ${device.device}`, secondary: `Serial ${device.path}` };
  }
  return {
    primary: `Node ${root.node}`,
    secondary: `Device ${device.device} · AdvA ${root.local.slice(0, 4)}…${root.local.slice(-4)}`,
  };
}
