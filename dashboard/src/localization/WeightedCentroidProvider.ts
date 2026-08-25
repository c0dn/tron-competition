import type {
  InvalidLocalizationInputResult,
  LocalizationAnchor,
  LocalizationInput,
  LocalizationProvider,
  LocalizationResult,
} from './LocalizationProvider';

const DEPLOYMENT_PATH_LOSS_EXPONENT = 2.0;
const MAX_CONTRIBUTORS = 4;
const REQUIRED_CONTRIBUTORS = 3;
const DEGENERATE_AREA_EPSILON = 1e-6;

interface Contributor {
  readonly anchor: LocalizationAnchor;
  readonly rssiDbm: number;
}

interface Centroid {
  readonly x: number;
  readonly y: number;
}

type UnknownRecord = Record<string, unknown>;

function isRecord(value: unknown): value is UnknownRecord {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

function compareAnchorIds(left: string, right: string): number {
  if (left < right) return -1;
  if (left > right) return 1;
  return 0;
}

function clampUnit(value: number): number {
  return Math.max(0, Math.min(1, value));
}

function normalizedDoubledTriangleArea(first: LocalizationAnchor, second: LocalizationAnchor, third: LocalizationAnchor): number {
  /*
   * This is algebraically equivalent to
   * abs(cross((aspect * x, y))) / aspect. Factoring out the positive aspect
   * prevents an intermediate cross-product overflow for extreme valid aspects.
   */
  const secondX = second.x - first.x;
  const secondY = second.y - first.y;
  const thirdX = third.x - first.x;
  const thirdY = third.y - first.y;
  return clampUnit(Math.abs((secondX * thirdY) - (secondY * thirdX)));
}

function isDegenerate(contributors: readonly Contributor[]): boolean {
  for (let first = 0; first < contributors.length - 2; first += 1) {
    for (let second = first + 1; second < contributors.length - 1; second += 1) {
      for (let third = second + 1; third < contributors.length; third += 1) {
        const area = normalizedDoubledTriangleArea(
          contributors[first].anchor,
          contributors[second].anchor,
          contributors[third].anchor,
        );
        if (area > DEGENERATE_AREA_EPSILON) return false;
      }
    }
  }
  return true;
}

function normalizedDistance(left: Centroid, right: Centroid, aspectRatio: number): number {
  const numerator = Math.hypot(
    aspectRatio * (left.x - right.x),
    left.y - right.y,
  );
  const denominator = Math.hypot(aspectRatio, 1);
  const distance = numerator / denominator;
  return clampUnit(distance);
}

export class WeightedCentroidProvider implements LocalizationProvider {
  readonly id = 'weighted-centroid';

  constructor(private readonly pathLossExponent = DEPLOYMENT_PATH_LOSS_EXPONENT) {
    if (!Number.isFinite(pathLossExponent) || pathLossExponent <= 0) {
      throw new RangeError('pathLossExponent must be a finite positive number.');
    }
  }

  estimate(input: LocalizationInput): LocalizationResult {
    const invalid = (reason: string): InvalidLocalizationInputResult => ({
      status: 'invalid_input',
      providerId: this.id,
      reason,
    });

    if (!isRecord(input)) return invalid('input must be an object.');
    if (!isRecord(input.coordinateSpace)
      || typeof input.coordinateSpace.aspectRatio !== 'number'
      || !Number.isFinite(input.coordinateSpace.aspectRatio)
      || input.coordinateSpace.aspectRatio <= 0) {
      return invalid('coordinateSpace.aspectRatio must be a finite positive number.');
    }
    if (!Array.isArray(input.anchors)) return invalid('anchors must be an array.');
    if (!Array.isArray(input.observations)) return invalid('observations must be an array.');

    const anchorsById = new Map<string, LocalizationAnchor>();
    for (let index = 0; index < input.anchors.length; index += 1) {
      const anchor = input.anchors[index];
      if (!isRecord(anchor)
        || typeof anchor.id !== 'string'
        || anchor.id.length === 0) {
        return invalid(`anchors[${index}].id must be a nonempty string.`);
      }
      if (typeof anchor.x !== 'number' || !Number.isFinite(anchor.x) || anchor.x < 0 || anchor.x > 1) {
        return invalid(`anchors[${index}].x must be a finite number in [0, 1].`);
      }
      if (typeof anchor.y !== 'number' || !Number.isFinite(anchor.y) || anchor.y < 0 || anchor.y > 1) {
        return invalid(`anchors[${index}].y must be a finite number in [0, 1].`);
      }
      if (anchorsById.has(anchor.id)) return invalid(`anchors[${index}].id must be unique.`);
      anchorsById.set(anchor.id, { id: anchor.id, x: anchor.x, y: anchor.y });
    }

    const observedAnchorIds = new Set<string>();
    const contributors: Contributor[] = [];
    for (let index = 0; index < input.observations.length; index += 1) {
      const observation = input.observations[index];
      if (!isRecord(observation) || typeof observation.anchorId !== 'string') {
        return invalid(`observations[${index}].anchorId must be a string.`);
      }
      if (observedAnchorIds.has(observation.anchorId)) {
        return invalid(`observations[${index}].anchorId must be unique.`);
      }
      observedAnchorIds.add(observation.anchorId);
      const anchor = anchorsById.get(observation.anchorId);
      if (!anchor) return invalid(`observations[${index}].anchorId must resolve to an anchor.`);
      if (typeof observation.rssiDbm !== 'number'
        || !Number.isFinite(observation.rssiDbm)
        || !Number.isInteger(observation.rssiDbm)
        || observation.rssiDbm < -127
        || observation.rssiDbm > -1) {
        return invalid(`observations[${index}].rssiDbm must be an integer in [-127, -1].`);
      }
      contributors.push({ anchor, rssiDbm: observation.rssiDbm });
    }

    const selected = [...contributors]
      .sort((left, right) => (right.rssiDbm - left.rssiDbm)
        || compareAnchorIds(left.anchor.id, right.anchor.id))
      .slice(0, MAX_CONTRIBUTORS);
    const contributorIds = selected.map((contributor) => contributor.anchor.id);

    if (selected.length < REQUIRED_CONTRIBUTORS) {
      return {
        status: 'insufficient',
        providerId: this.id,
        contributors: contributorIds,
        required: REQUIRED_CONTRIBUTORS,
      };
    }

    const centroid = this.weightedCentroid(selected);
    if (!centroid) return invalid('weighted centroid is not finite.');

    let normalizedSpread: number | null = null;
    if (selected.length === MAX_CONTRIBUTORS) {
      const leaveOneOutCentroids: Centroid[] = [];
      for (let excluded = 0; excluded < selected.length; excluded += 1) {
        const estimate = this.weightedCentroid(selected.filter((_, index) => index !== excluded));
        if (!estimate) return invalid('leave-one-out centroid is not finite.');
        leaveOneOutCentroids.push(estimate);
      }

      let maximumDistance = 0;
      for (let left = 0; left < leaveOneOutCentroids.length - 1; left += 1) {
        for (let right = left + 1; right < leaveOneOutCentroids.length; right += 1) {
          const distance = normalizedDistance(
            leaveOneOutCentroids[left],
            leaveOneOutCentroids[right],
            input.coordinateSpace.aspectRatio,
          );
          if (!Number.isFinite(distance)) return invalid('normalized spread is not finite.');
          maximumDistance = Math.max(maximumDistance, distance);
        }
      }
      normalizedSpread = clampUnit(maximumDistance);
    }

    return {
      status: 'estimated',
      providerId: this.id,
      x: centroid.x,
      y: centroid.y,
      contributors: contributorIds,
      geometryWarning: isDegenerate(selected),
      normalizedSpread,
    };
  }

  private weightedCentroid(contributors: readonly Contributor[]): Centroid | null {
    const strongestRssi = Math.max(...contributors.map((contributor) => contributor.rssiDbm));
    let totalWeight = 0;
    let weightedX = 0;
    let weightedY = 0;

    for (const contributor of contributors) {
      const weight = 10 ** ((contributor.rssiDbm - strongestRssi) / (5 * this.pathLossExponent));
      totalWeight += weight;
      weightedX += weight * contributor.anchor.x;
      weightedY += weight * contributor.anchor.y;
    }

    const x = weightedX / totalWeight;
    const y = weightedY / totalWeight;
    if (!Number.isFinite(x) || !Number.isFinite(y)) return null;
    return { x: clampUnit(x), y: clampUnit(y) };
  }
}
