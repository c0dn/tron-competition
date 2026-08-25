import type { LayoutPosition, LayoutReadyResponse } from '../lib/api';
import { floorplanAspect } from '../state/layout';
import type { LogicalEvent } from '../state/store';
import type { LocalizationProvider, LocalizationResult } from './LocalizationProvider';

/**
 * Presentation-only location states. Provider-specific states deliberately do
 * not cross this boundary: in particular, invalid provider input is rendered
 * as insufficient without coordinates.
 */
export type IncidentLocalizationView =
  | CollectingIncidentLocalizationView
  | InsufficientIncidentLocalizationView
  | BallparkIncidentLocalizationView;

interface IncidentLocalizationViewBase {
  readonly logicalEvent: LogicalEvent;
  readonly contributorIds: ReadonlyArray<string>;
  readonly contributorCount: number;
}

export interface CollectingIncidentLocalizationView extends IncidentLocalizationViewBase {
  readonly status: 'collecting';
}

export interface InsufficientIncidentLocalizationView extends IncidentLocalizationViewBase {
  readonly status: 'insufficient';
}

export interface BallparkIncidentLocalizationView extends IncidentLocalizationViewBase {
  readonly status: 'ballpark';
  readonly geometryWarning: boolean;
  readonly normalizedSpread: number | null;
  readonly x: number;
  readonly y: number;
}

interface JoinedEvidence {
  readonly anchorId: string;
  readonly rssiDbm: number;
}

function strongestEvidenceByObserver(logicalEvent: LogicalEvent, positions: readonly LayoutPosition[]): JoinedEvidence[] {
  const positionsByAdva = new Map(positions.map((position) => [position.adva, position]));
  const evidenceByObserver = new Map<string, JoinedEvidence>();

  for (const evidence of logicalEvent.evidence) {
    if (evidence.observerRssiDbm === null || !positionsByAdva.has(evidence.observer)) continue;
    const current = evidenceByObserver.get(evidence.observer);
    if (!current || evidence.observerRssiDbm > current.rssiDbm) {
      evidenceByObserver.set(evidence.observer, {
        anchorId: evidence.observer,
        rssiDbm: evidence.observerRssiDbm,
      });
    }
  }

  return [...evidenceByObserver.values()]
    .sort((left, right) => (left.anchorId < right.anchorId ? -1 : left.anchorId > right.anchorId ? 1 : 0));
}

function insufficient(
  logicalEvent: LogicalEvent,
  contributorIds: readonly string[],
): InsufficientIncidentLocalizationView {
  return {
    logicalEvent,
    status: 'insufficient',
    contributorIds,
    contributorCount: contributorIds.length,
  };
}

function fromProvider(
  logicalEvent: LogicalEvent,
  fallbackContributorIds: readonly string[],
  result: LocalizationResult,
): IncidentLocalizationView {
  if (result.status === 'estimated') {
    return {
      logicalEvent,
      status: 'ballpark',
      contributorIds: result.contributors,
      contributorCount: result.contributors.length,
      geometryWarning: result.geometryWarning,
      normalizedSpread: result.normalizedSpread,
      x: result.x,
      y: result.y,
    };
  }
  if (result.status === 'insufficient') {
    return insufficient(logicalEvent, result.contributors);
  }

  // `invalid_input` and its reason are intentionally internal to the provider
  // boundary. The presentation fails closed with no marker-capable fields.
  return insufficient(logicalEvent, fallbackContributorIds);
}

/**
 * Derive one UI-ready incident location from immutable logical evidence and a
 * confirmed persisted layout. Current GTT membership is intentionally absent:
 * it cannot invalidate an observer that had an explicitly persisted position.
 */
export function deriveIncidentLocalization(
  logicalEvent: LogicalEvent,
  now: number,
  layout: LayoutReadyResponse | null,
  provider: LocalizationProvider,
): IncidentLocalizationView {
  if (now < logicalEvent.collectUntil) {
    return {
      logicalEvent,
      status: 'collecting',
      contributorIds: [],
      contributorCount: 0,
    };
  }

  const positions = layout?.positions ?? [];
  const joinedEvidence = strongestEvidenceByObserver(logicalEvent, positions);
  const positionsByAdva = new Map(positions.map((position) => [position.adva, position]));
  const contributorIds = joinedEvidence.map((evidence) => evidence.anchorId);
  const result = provider.estimate({
    anchors: joinedEvidence.map((evidence) => {
      const position = positionsByAdva.get(evidence.anchorId);
      if (!position) throw new Error('joined evidence must have a persisted position.');
      return { id: evidence.anchorId, x: position.x, y: position.y };
    }),
    observations: joinedEvidence,
    coordinateSpace: { aspectRatio: floorplanAspect(layout) },
  });

  return fromProvider(logicalEvent, contributorIds, result);
}
