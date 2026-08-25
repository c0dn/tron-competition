import { describe, expect, it, vi } from 'vitest';
import type { LocalizationInput, LocalizationProvider, LocalizationResult } from './LocalizationProvider';
import { deriveIncidentLocalization } from './IncidentLocalization';
import { eventRecord, layoutReady } from '../test/fixtures';
import type { LogicalEvent } from '../state/store';

function logicalEvent(overrides: Partial<LogicalEvent> = {}): LogicalEvent {
  const record = eventRecord();
  return {
    key: '7:00002a',
    fingerprint: '1:3:75:2400:86:42',
    record,
    evidence: [{
      root: record.root,
      observer: record.observer,
      path: record.path,
      device: record.device,
      observerRssiDbm: -50,
      sampleCount: 1,
    }],
    evidenceOverflowCount: 0,
    evidenceSaturated: false,
    conflict: false,
    firstReceivedAt: 1_000,
    collectUntil: 3_000,
    ...overrides,
  };
}

function provider(result: LocalizationResult) {
  return {
    id: 'test-provider',
    estimate: vi.fn(() => result),
  } satisfies LocalizationProvider;
}

describe('incident localization composition', () => {
  it('waits through deadline minus one without calling the provider, then derives at the deadline', () => {
    const logical = logicalEvent();
    const testProvider = provider({
      status: 'insufficient', providerId: 'test-provider', contributors: [], required: 3,
    });

    expect(deriveIncidentLocalization(logical, logical.collectUntil - 1, null, testProvider)).toMatchObject({
      status: 'collecting', contributorCount: 0,
    });
    expect(testProvider.estimate).not.toHaveBeenCalled();

    expect(deriveIncidentLocalization(logical, logical.collectUntil, null, testProvider)).toMatchObject({
      status: 'insufficient', contributorCount: 0,
    });
    expect(testProvider.estimate).toHaveBeenCalledOnce();
  });

  it('joins only unique positioned observers with RSSI, including persisted observers absent from GTT', () => {
    const positionedOffGtt = '0102545678c1';
    const secondPositioned = '0102545678c2';
    const thirdPositioned = '0102545678c3';
    const logical = logicalEvent({
      evidence: [
        { root: 'a', observer: positionedOffGtt, path: 'local', device: 1, observerRssiDbm: -80, sampleCount: 1 },
        { root: 'b', observer: positionedOffGtt, path: 'tavrn', device: 2, observerRssiDbm: -40, sampleCount: 2 },
        { root: 'a', observer: secondPositioned, path: 'local', device: 3, observerRssiDbm: null, sampleCount: 1 },
        { root: 'a', observer: thirdPositioned, path: 'local', device: 4, observerRssiDbm: -55, sampleCount: 1 },
        { root: 'a', observer: '0102545678ff', path: 'local', device: 5, observerRssiDbm: -45, sampleCount: 1 },
      ],
    });
    const testProvider = provider({
      status: 'insufficient', providerId: 'test-provider', contributors: [positionedOffGtt, thirdPositioned], required: 3,
    });

    const view = deriveIncidentLocalization(logical, logical.collectUntil, layoutReady({ positions: [
      { adva: positionedOffGtt, x: 0.1, y: 0.2 },
      { adva: secondPositioned, x: 0.3, y: 0.4 },
      { adva: thirdPositioned, x: 0.5, y: 0.6 },
    ] }), testProvider);

    expect(view).toMatchObject({ status: 'insufficient', contributorIds: [positionedOffGtt, thirdPositioned], contributorCount: 2 });
    expect(testProvider.estimate).toHaveBeenCalledWith({
      anchors: [
        { id: positionedOffGtt, x: 0.1, y: 0.2 },
        { id: thirdPositioned, x: 0.5, y: 0.6 },
      ],
      observations: [
        { anchorId: positionedOffGtt, rssiDbm: -40 },
        { anchorId: thirdPositioned, rssiDbm: -55 },
      ],
      coordinateSpace: { aspectRatio: 16 / 9 },
    });
  });

  it('recomputes numeric blank and uploaded-image aspect ratios', () => {
    const logical = logicalEvent();
    const calls: LocalizationInput[] = [];
    const testProvider: LocalizationProvider = {
      id: 'test-provider',
      estimate: (input) => {
        calls.push(input);
        return { status: 'insufficient', providerId: 'test-provider', contributors: [], required: 3 };
      },
    };

    deriveIncidentLocalization(logical, logical.collectUntil, layoutReady(), testProvider);
    deriveIncidentLocalization(logical, logical.collectUntil, layoutReady({ floorplan: {
      sha256: 'a'.repeat(64), mime: 'image/png', width: 1200, height: 800, url: `/api/floorplan/${'a'.repeat(64)}`,
    } }), testProvider);

    expect(calls.map((input) => input.coordinateSpace.aspectRatio)).toEqual([16 / 9, 1.5]);
  });

  it('maps provider invalid input to visible insufficient without coordinates or an exposed reason', () => {
    const logical = logicalEvent();
    const view = deriveIncidentLocalization(logical, logical.collectUntil, null, provider({
      status: 'invalid_input', providerId: 'test-provider', reason: 'private diagnostic',
    }));

    expect(view).toEqual(expect.objectContaining({ status: 'insufficient', contributorIds: [], contributorCount: 0 }));
    expect(view).not.toHaveProperty('x');
    expect(view).not.toHaveProperty('y');
    expect(view).not.toHaveProperty('reason');
  });

  it('does not mutate logical evidence, confirmed positions, or provider input sources', () => {
    const logical = Object.freeze(logicalEvent({ evidence: Object.freeze([
      Object.freeze({ root: 'a', observer: '0102545678c1', path: 'local' as const, device: 1, observerRssiDbm: -50, sampleCount: 1 }),
      Object.freeze({ root: 'a', observer: '0102545678c2', path: 'local' as const, device: 2, observerRssiDbm: -60, sampleCount: 1 }),
      Object.freeze({ root: 'a', observer: '0102545678c3', path: 'local' as const, device: 3, observerRssiDbm: -70, sampleCount: 1 }),
    ]) as unknown as LogicalEvent['evidence'] })) as LogicalEvent;
    const layout = Object.freeze(layoutReady({ positions: Object.freeze([
      Object.freeze({ adva: '0102545678c1', x: 0.1, y: 0.2 }),
      Object.freeze({ adva: '0102545678c2', x: 0.3, y: 0.4 }),
      Object.freeze({ adva: '0102545678c3', x: 0.5, y: 0.6 }),
    ]) as unknown as { adva: string; x: number; y: number }[] })) as ReturnType<typeof layoutReady>;
    const before = structuredClone({ logical, layout });
    const testProvider = provider({
      status: 'estimated', providerId: 'test-provider', x: 0.2, y: 0.3,
      contributors: ['0102545678c1', '0102545678c2', '0102545678c3'], geometryWarning: false, normalizedSpread: null,
    });

    expect(deriveIncidentLocalization(logical, logical.collectUntil, layout, testProvider).status).toBe('ballpark');
    expect({ logical, layout }).toEqual(before);
  });
});
