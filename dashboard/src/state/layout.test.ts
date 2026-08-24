import { describe, expect, it } from 'vitest';
import { clampCoordinate, floorplanAspect, isAcceptedFloorplan, MAX_FLOORPLAN_BYTES, removePosition, setPosition, sortedPositions } from './layout';
import { layoutReady } from '../test/fixtures';

describe('floorplan layout state', () => {
  it('clamps and sorts explicit normalized coordinates without fabricating positions', () => {
    const positions = sortedPositions([
      { adva: '0102545678c2', x: 2, y: -1 },
      { adva: '0102545678c1', x: 0.25, y: 0.75 },
    ]);
    expect(positions).toEqual([
      { adva: '0102545678c1', x: 0.25, y: 0.75 },
      { adva: '0102545678c2', x: 1, y: 0 },
    ]);
    expect(setPosition(positions, { adva: '0102545678c1', x: 0.5, y: 0.5 })).toHaveLength(2);
    expect(removePosition(positions, '0102545678c1')).toEqual([{ adva: '0102545678c2', x: 1, y: 0 }]);
    expect(clampCoordinate(Number.NaN)).toBe(0);
  });

  it('uses a fixed blank frame and an uploaded image intrinsic aspect ratio', () => {
    expect(floorplanAspect(layoutReady())).toBe('16 / 9');
    expect(floorplanAspect(layoutReady({ floorplan: {
      sha256: 'a'.repeat(64), mime: 'image/webp', width: 2000, height: 1000, url: `/api/floorplan/${'a'.repeat(64)}`,
    } }))).toBe('2000 / 1000');
    expect(isAcceptedFloorplan({ type: 'image/png', size: MAX_FLOORPLAN_BYTES })).toBe(true);
    expect(isAcceptedFloorplan({ type: 'image/svg+xml', size: 10 })).toBe(false);
    expect(isAcceptedFloorplan({ type: 'image/jpeg', size: MAX_FLOORPLAN_BYTES + 1 })).toBe(false);
  });
});
