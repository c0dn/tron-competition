export interface LocalizationAnchor {
  readonly id: string;
  readonly x: number;
  readonly y: number;
}

export interface LocalizationObservation {
  readonly anchorId: string;
  readonly rssiDbm: number;
}

export interface LocalizationCoordinateSpace {
  readonly aspectRatio: number;
}

export interface LocalizationInput {
  readonly anchors: ReadonlyArray<LocalizationAnchor>;
  readonly observations: ReadonlyArray<LocalizationObservation>;
  readonly coordinateSpace: LocalizationCoordinateSpace;
}

export interface EstimatedLocalizationResult {
  readonly status: 'estimated';
  readonly providerId: string;
  readonly x: number;
  readonly y: number;
  readonly contributors: ReadonlyArray<string>;
  readonly geometryWarning: boolean;
  readonly normalizedSpread: number | null;
}

export interface InsufficientLocalizationResult {
  readonly status: 'insufficient';
  readonly providerId: string;
  readonly contributors: ReadonlyArray<string>;
  readonly required: 3;
}

export interface InvalidLocalizationInputResult {
  readonly status: 'invalid_input';
  readonly providerId: string;
  readonly reason: string;
}

export type LocalizationResult =
  | EstimatedLocalizationResult
  | InsufficientLocalizationResult
  | InvalidLocalizationInputResult;

export interface LocalizationProvider {
  readonly id: string;

  estimate(input: LocalizationInput): LocalizationResult;
}
