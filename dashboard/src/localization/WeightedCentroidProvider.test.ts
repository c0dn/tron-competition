import { describe, expect, it } from 'vitest';
import type {
  EstimatedLocalizationResult,
  LocalizationAnchor,
  LocalizationInput,
  LocalizationObservation,
} from './LocalizationProvider';
import { WeightedCentroidProvider } from './WeightedCentroidProvider';

const provider = new WeightedCentroidProvider();

function input(
  anchors: readonly LocalizationAnchor[],
  observations: readonly LocalizationObservation[],
  aspectRatio = 1,
): LocalizationInput {
  return {
    anchors,
    observations,
    coordinateSpace: { aspectRatio },
  };
}

function estimated(value: LocalizationInput, currentProvider = provider): EstimatedLocalizationResult {
  const result = currentProvider.estimate(value);
  if (result.status !== 'estimated') {
    throw new Error(`expected an estimate, received ${result.status}`);
  }
  return result;
}

const triangleAnchors: readonly LocalizationAnchor[] = [
  { id: 'anchor-a', x: 0, y: 0 },
  { id: 'anchor-b', x: 1, y: 0 },
  { id: 'anchor-c', x: 0, y: 1 },
];

const triangleObservations: readonly LocalizationObservation[] = [
  { anchorId: 'anchor-a', rssiDbm: -40 },
  { anchorId: 'anchor-b', rssiDbm: -50 },
  { anchorId: 'anchor-c', rssiDbm: -60 },
];

describe('WeightedCentroidProvider', () => {
  it('requires one contributor and places a one-observer estimate exactly at its anchor', () => {
    const insufficient = provider.estimate(input(
      [],
      [],
    ));
    expect(insufficient).toEqual({
      status: 'insufficient',
      providerId: 'weighted-centroid',
      contributors: [],
      required: 1,
    });

    const estimate = estimated(input(
      [{ id: 'anchor-a', x: 0.25, y: 0.75 }],
      [{ anchorId: 'anchor-a', rssiDbm: -60 }],
    ));
    expect(estimate).toMatchObject({
      status: 'estimated',
      providerId: 'weighted-centroid',
      x: 0.25,
      y: 0.75,
      contributors: ['anchor-a'],
      geometryWarning: false,
      normalizedSpread: null,
    });
  });

  it('uses the deployment n=2.0 relative weights and accepts only finite positive constructor exponents', () => {
    const weights = [1, 0.1, 0.01];
    const totalWeight = 1.11;
    const defaultEstimate = estimated(input(triangleAnchors, triangleObservations));
    const explicitlyConfigured = estimated(
      input(triangleAnchors, triangleObservations),
      new WeightedCentroidProvider(2.0),
    );

    expect(defaultEstimate.x).toBeCloseTo(weights[1] / totalWeight, 12);
    expect(defaultEstimate.y).toBeCloseTo(weights[2] / totalWeight, 12);
    expect(explicitlyConfigured.x).toBeCloseTo(defaultEstimate.x, 15);
    expect(explicitlyConfigured.y).toBeCloseTo(defaultEstimate.y, 15);
    expect(estimated(input(triangleAnchors, triangleObservations), new WeightedCentroidProvider(1)).x)
      .toBeCloseTo(0.01 / 1.0101, 12);

    expect(() => new WeightedCentroidProvider(0)).toThrow(RangeError);
    expect(() => new WeightedCentroidProvider(Number.NaN)).toThrow(RangeError);
    expect(() => new WeightedCentroidProvider(Number.POSITIVE_INFINITY)).toThrow(RangeError);
  });

  it('uses all sixteen retained observers, orders contributors deterministically, and rejects overflow input', () => {
    const anchors = Array.from({ length: 16 }, (_, index) => ({
      id: `anchor-${index.toString().padStart(2, '0')}`,
      x: index / 15,
      y: index % 2,
    }));
    const observations = anchors.map((anchor) => ({ anchorId: anchor.id, rssiDbm: -60 }));
    const result = estimated(input([...anchors].reverse(), [...observations].reverse()));
    const reordered = estimated(input(anchors, observations));

    expect(result.contributors).toEqual(anchors.map((anchor) => anchor.id));
    expect(result.x).toBeCloseTo(0.5, 12);
    expect(result.y).toBeCloseTo(0.5, 12);
    expect(result.normalizedSpread).not.toBeNull();
    expect(Number.isFinite(result.normalizedSpread)).toBe(true);
    expect(result.normalizedSpread).toBeGreaterThanOrEqual(0);
    expect(result.normalizedSpread).toBeLessThanOrEqual(1);
    expect(reordered).toEqual(result);

    const overflowAnchor = { id: 'anchor-16', x: 0.5, y: 0.5 };
    expect(provider.estimate(input(
      [...anchors, overflowAnchor],
      [...observations, { anchorId: overflowAnchor.id, rssiDbm: -60 }],
    ))).toEqual({
      status: 'invalid_input',
      providerId: 'weighted-centroid',
      reason: 'observations must contain at most 16 entries.',
    });
  });

  it('warns only when every selected contributor triangle is degenerate at the frozen epsilon', () => {
    const collinear = estimated(input(
      [
        { id: 'a', x: 0, y: 0.5 },
        { id: 'b', x: 0.5, y: 0.5 },
        { id: 'c', x: 1, y: 0.5 },
      ],
      [
        { anchorId: 'a', rssiDbm: -50 },
        { anchorId: 'b', rssiDbm: -50 },
        { anchorId: 'c', rssiDbm: -50 },
      ],
    ));
    const nondegenerate = estimated(input(triangleAnchors, triangleObservations));
    const single = estimated(input(triangleAnchors.slice(0, 1), triangleObservations.slice(0, 1)));
    const pair = estimated(input(triangleAnchors.slice(0, 2), triangleObservations.slice(0, 2)));
    const fourWithAnUsableTriangle = estimated(input(
      [
        { id: 'a', x: 0, y: 0 },
        { id: 'b', x: 1, y: 0 },
        { id: 'c', x: 0.5, y: 0 },
        { id: 'd', x: 0.5, y: 1 },
      ],
      [
        { anchorId: 'a', rssiDbm: -50 },
        { anchorId: 'b', rssiDbm: -50 },
        { anchorId: 'c', rssiDbm: -50 },
        { anchorId: 'd', rssiDbm: -50 },
      ],
    ));
    const epsilonObservations: readonly LocalizationObservation[] = [
      { anchorId: 'a', rssiDbm: -50 },
      { anchorId: 'b', rssiDbm: -50 },
      { anchorId: 'c', rssiDbm: -50 },
    ];
    const atEpsilonAnchors: readonly LocalizationAnchor[] = [
      { id: 'a', x: 0, y: 0 },
      { id: 'b', x: 1, y: 0 },
      { id: 'c', x: 0, y: 1e-6 },
    ];
    const aboveEpsilonAnchors: readonly LocalizationAnchor[] = [
      { id: 'a', x: 0, y: 0 },
      { id: 'b', x: 1, y: 0 },
      { id: 'c', x: 0, y: 1.000001e-6 },
    ];

    expect(collinear.geometryWarning).toBe(true);
    expect(nondegenerate.geometryWarning).toBe(false);
    expect(single.geometryWarning).toBe(false);
    expect(pair.geometryWarning).toBe(false);
    expect(fourWithAnUsableTriangle.geometryWarning).toBe(false);
    for (const aspectRatio of [0.25, 2.5, Number.MIN_VALUE, Number.MAX_VALUE]) {
      expect(estimated(input(atEpsilonAnchors, epsilonObservations, aspectRatio)).geometryWarning).toBe(true);
      expect(estimated(input(aboveEpsilonAnchors, epsilonObservations, aspectRatio)).geometryWarning).toBe(false);
    }
  });

  it('uses every leave-one-out centroid and exact aspect-normalized pairwise spread for two or more contributors', () => {
    const anchors: readonly LocalizationAnchor[] = [
      { id: 'a', x: 0, y: 0 },
      { id: 'b', x: 1, y: 0 },
      { id: 'c', x: 0, y: 1 },
      { id: 'd', x: 0.2, y: 0.6 },
    ];
    const observations = anchors.map((anchor) => ({ anchorId: anchor.id, rssiDbm: -60 }));
    const aspectRatio = 2;
    const result = estimated(input(anchors, observations, aspectRatio));
    const leaveOneOut = [
      { x: (1 + 0 + 0.2) / 3, y: (0 + 1 + 0.6) / 3 },
      { x: (0 + 0 + 0.2) / 3, y: (0 + 1 + 0.6) / 3 },
      { x: (0 + 1 + 0.2) / 3, y: (0 + 0 + 0.6) / 3 },
      { x: (0 + 1 + 0) / 3, y: (0 + 0 + 1) / 3 },
    ];
    const pairwiseDistances: number[] = [];
    for (let left = 0; left < leaveOneOut.length - 1; left += 1) {
      for (let right = left + 1; right < leaveOneOut.length; right += 1) {
        pairwiseDistances.push(Math.hypot(
          aspectRatio * (leaveOneOut[left].x - leaveOneOut[right].x),
          leaveOneOut[left].y - leaveOneOut[right].y,
        ) / Math.hypot(aspectRatio, 1));
      }
    }

    expect(pairwiseDistances).toHaveLength(6);
    expect(result.normalizedSpread).toBeCloseTo(Math.max(...pairwiseDistances), 12);

    const pair = estimated(input(
      triangleAnchors.slice(0, 2),
      triangleObservations.slice(0, 2),
    ));
    expect(pair.x).toBeCloseTo(1 / 11, 12);
    expect(pair.y).toBe(0);
    expect(pair.normalizedSpread).toBeCloseTo(1 / Math.sqrt(2), 12);

    const equalTriangle = estimated(input(
      triangleAnchors,
      triangleAnchors.map((anchor) => ({ anchorId: anchor.id, rssiDbm: -60 })),
    ));
    expect(equalTriangle.normalizedSpread).toBeCloseTo(0.5, 12);
  });

  it('keeps centroids and spread bounded for finite extreme aspect ratios', () => {
    const corners: readonly LocalizationAnchor[] = [
      { id: 'a', x: 0, y: 0 },
      { id: 'b', x: 1, y: 0 },
      { id: 'c', x: 0, y: 1 },
      { id: 'd', x: 1, y: 1 },
    ];
    const observations = corners.map((anchor) => ({ anchorId: anchor.id, rssiDbm: -60 }));

    for (const aspectRatio of [Number.MIN_VALUE, Number.MAX_VALUE]) {
      const result = estimated(input(corners, observations, aspectRatio));
      expect(result.x).toBeGreaterThanOrEqual(0);
      expect(result.x).toBeLessThanOrEqual(1);
      expect(result.y).toBeGreaterThanOrEqual(0);
      expect(result.y).toBeLessThanOrEqual(1);
      expect(result.normalizedSpread).not.toBeNull();
      expect(Number.isFinite(result.normalizedSpread)).toBe(true);
      expect(result.normalizedSpread).toBeGreaterThanOrEqual(0);
      expect(result.normalizedSpread).toBeLessThanOrEqual(1);
    }
  });

  it('does not mutate frozen input arrays, anchor objects, observation objects, or coordinate space', () => {
    const frozenInput = Object.freeze({
      anchors: Object.freeze([
        Object.freeze({ id: 'b', x: 1, y: 0 }),
        Object.freeze({ id: 'a', x: 0, y: 0 }),
        Object.freeze({ id: 'c', x: 0, y: 1 }),
      ]),
      observations: Object.freeze([
        Object.freeze({ anchorId: 'b', rssiDbm: -60 }),
        Object.freeze({ anchorId: 'a', rssiDbm: -40 }),
        Object.freeze({ anchorId: 'c', rssiDbm: -50 }),
      ]),
      coordinateSpace: Object.freeze({ aspectRatio: 1.5 }),
    }) satisfies LocalizationInput;
    const before = structuredClone(frozenInput);

    expect(provider.estimate(frozenInput).status).toBe('estimated');
    expect(frozenInput).toEqual(before);
  });

  it('fails closed for malformed and invalid inputs without throwing', () => {
    const valid = input(triangleAnchors, triangleObservations);
    const invalidInputs: readonly unknown[] = [
      { ...valid, coordinateSpace: { aspectRatio: 0 } },
      { ...valid, coordinateSpace: { aspectRatio: -1 } },
      { ...valid, coordinateSpace: { aspectRatio: Number.NaN } },
      { ...valid, coordinateSpace: { aspectRatio: Number.POSITIVE_INFINITY } },
      { ...valid, anchors: [{ ...triangleAnchors[0], x: Number.NaN }, ...triangleAnchors.slice(1)] },
      { ...valid, anchors: [{ ...triangleAnchors[0], x: Number.POSITIVE_INFINITY }, ...triangleAnchors.slice(1)] },
      { ...valid, anchors: [{ ...triangleAnchors[0], x: -0.001 }, ...triangleAnchors.slice(1)] },
      { ...valid, anchors: [{ ...triangleAnchors[0], y: 1.001 }, ...triangleAnchors.slice(1)] },
      { ...valid, anchors: [{ ...triangleAnchors[0], id: '' }, ...triangleAnchors.slice(1)] },
      { ...valid, anchors: [{ ...triangleAnchors[0] }, { ...triangleAnchors[1], id: 'anchor-a' }, triangleAnchors[2]] },
      { ...valid, observations: [{ ...triangleObservations[0] }, { ...triangleObservations[1], anchorId: 'anchor-a' }, triangleObservations[2]] },
      { ...valid, observations: [{ ...triangleObservations[0], anchorId: 'missing' }, ...triangleObservations.slice(1)] },
      { ...valid, observations: [{ ...triangleObservations[0], rssiDbm: Number.NaN }, ...triangleObservations.slice(1)] },
      { ...valid, observations: [{ ...triangleObservations[0], rssiDbm: -1.5 }, ...triangleObservations.slice(1)] },
      { ...valid, observations: [{ ...triangleObservations[0], rssiDbm: -128 }, ...triangleObservations.slice(1)] },
      { ...valid, observations: [{ ...triangleObservations[0], rssiDbm: 0 }, ...triangleObservations.slice(1)] },
      { anchors: null, observations: [], coordinateSpace: { aspectRatio: 1 } },
    ];

    for (const invalidInput of invalidInputs) {
      const result = provider.estimate(invalidInput as LocalizationInput);
      expect(result.status).toBe('invalid_input');
      if (result.status === 'invalid_input') {
        expect(result.providerId).toBe('weighted-centroid');
        expect(result.reason).not.toBe('');
      }
    }
  });
});
