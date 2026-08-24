import type { LayoutPosition, LayoutReadyResponse, LayoutResponse } from '../lib/api';

export const MAX_FLOORPLAN_BYTES = 5 * 1024 * 1024;
export const SMALL_KEYBOARD_STEP = 0.01;
export const LARGE_KEYBOARD_STEP = 0.05;

export type LayoutRequestPhase = 'loading' | 'ready' | 'saving' | 'error';

export interface LayoutState {
  phase: LayoutRequestPhase;
  layout: LayoutReadyResponse | null;
  message: string;
}

export const initialLayoutState: LayoutState = {
  phase: 'loading',
  layout: null,
  message: 'Loading floorplan.',
};

export function readyLayout(layout: LayoutResponse): LayoutReadyResponse | null {
  return layout.status === 'ready' ? layout : null;
}

export function clampCoordinate(value: number): number {
  if (!Number.isFinite(value)) return 0;
  return Math.max(0, Math.min(1, value));
}

export function sortedPositions(positions: readonly LayoutPosition[]): LayoutPosition[] {
  return [...positions]
    .map((position) => ({ ...position, x: clampCoordinate(position.x), y: clampCoordinate(position.y) }))
    .sort((left, right) => left.adva.localeCompare(right.adva));
}

export function setPosition(positions: readonly LayoutPosition[], position: LayoutPosition): LayoutPosition[] {
  return sortedPositions([...positions.filter((item) => item.adva !== position.adva), position]);
}

export function removePosition(positions: readonly LayoutPosition[], adva: string): LayoutPosition[] {
  return sortedPositions(positions.filter((position) => position.adva !== adva));
}

export function coordinateText(position: LayoutPosition | undefined): string {
  return position ? `${position.x.toFixed(2)}, ${position.y.toFixed(2)}` : 'Unpositioned';
}

export function floorplanAspect(layout: LayoutReadyResponse | null): string {
  return layout?.floorplan ? `${layout.floorplan.width} / ${layout.floorplan.height}` : '16 / 9';
}

export function isAcceptedFloorplan(file: Pick<File, 'type' | 'size'>): file is File & { type: 'image/png' | 'image/jpeg' | 'image/webp' } {
  return (file.type === 'image/png' || file.type === 'image/jpeg' || file.type === 'image/webp')
    && file.size <= MAX_FLOORPLAN_BYTES;
}
