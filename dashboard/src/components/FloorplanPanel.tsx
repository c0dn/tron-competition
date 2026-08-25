import { useCallback, useEffect, useRef, useState, type MouseEvent as ReactMouseEvent, type PointerEvent as ReactPointerEvent } from 'react';
import type { GttEntry, LayoutPosition, LayoutReadyResponse, RootRecord } from '../lib/api';
import {
  ApiError,
  getLayout,
  LayoutConflictError,
  postGtt,
  putLayout,
  removeFloorplan,
  uploadFloorplan,
} from '../lib/api';
import { formatAge } from '../lib/format';
import { gatewayDevice, gatewayEntryLabel, gatewayTopology, type GatewayTopologyIssue } from '../state/devices';
import type { IncidentLocalizationView } from '../localization/IncidentLocalization';
import {
  clampCoordinate,
  coordinateText,
  floorplanAspect,
  initialLayoutState,
  isAcceptedFloorplan,
  LARGE_KEYBOARD_STEP,
  readyLayout,
  removePosition,
  setPosition,
  SMALL_KEYBOARD_STEP,
  sortedPositions,
  type LayoutState,
} from '../state/layout';
import type { HealthDevice } from '../lib/api';

export const GTT_REFRESH_INTERVAL_MS = 10_000;
export const GTT_REQUEST_TIMEOUT_MS = 5_000;
export const GTT_CONFIRMATION_TIMEOUT_MS = 10_000;
export const LAYOUT_READ_TIMEOUT_MS = 10_000;
export const LAYOUT_MUTATION_TIMEOUT_MS = 10_000;

interface FloorplanPanelProps {
  devices: HealthDevice[];
  healthEpoch: number | null;
  currentEpoch: number;
  healthCurrentCursor: number | null;
  rootRecords: Record<number, RootRecord>;
  now: number;
  healthAuthoritative?: boolean;
  gttRefreshIntervalMs?: number;
  gttRequestTimeoutMs?: number;
  gttConfirmationTimeoutMs?: number;
  layoutReadTimeoutMs?: number;
  layoutMutationTimeoutMs?: number;
  incidentViews?: readonly IncidentLocalizationView[];
  onLayoutChange?: (layout: LayoutReadyResponse | null) => void;
}

type PositionTransform = (positions: readonly LayoutPosition[]) => LayoutPosition[];

interface DragState {
  adva: string;
  pointerId: number;
}

interface ActiveGttRequest {
  generation: number;
  epoch: number;
  controller: AbortController;
  httpTimer: number;
  confirmationTimer: number | null;
  httpTimedOut: boolean;
  httpAccepted: boolean;
}

interface ActiveLayoutMutation {
  id: number;
  controller: AbortController;
  timer: number;
  timedOut: boolean;
}

interface ActiveLayoutRead {
  id: number;
  controller: AbortController;
  timer: number;
  mutationId: number;
  timedOut: boolean;
}

const FLOORPLAN_NODE_EDGE_INSET_PX = 28;

function messageFor(error: unknown): string {
  return error instanceof Error ? error.message : 'Unknown floorplan error.';
}

function topologyMessage(issue: GatewayTopologyIssue): string {
  switch (issue) {
    case 'gateway_missing': return 'Gateway topology unavailable. Connect the sole Gateway.';
    case 'gateway_duplicate': return 'Gateway topology unavailable. More than one physical owner was reported.';
    case 'gateway_disconnected': return 'Gateway topology unavailable. Reconnect the Gateway.';
    case 'health_unavailable': return 'Gateway topology unavailable while Gateway health is refreshing.';
    case 'gtt_missing': return 'Gateway topology unavailable. Request a complete GTT.';
    case 'gtt_self_missing': return 'Gateway topology unavailable. Gateway self entry is missing.';
    case 'gtt_self_duplicate': return 'Gateway topology unavailable. Gateway self entry is duplicated.';
    case 'gtt_root_mismatch': return 'Gateway topology unavailable. Gateway identity does not match status.';
  }
}

function freshnessText(entry: GttEntry): string {
  return entry.freshness.replaceAll('_', ' ');
}

function departureText(entry: GttEntry): string {
  return entry.departed === 'true' ? 'departed' : entry.departed === 'false' ? 'present' : entry.departed.replaceAll('_', ' ');
}

function positionFor(positions: readonly LayoutPosition[], adva: string): LayoutPosition | undefined {
  return positions.find((position) => position.adva === adva);
}

function pointAt(event: ReactPointerEvent<HTMLElement> | ReactMouseEvent<HTMLElement>, target: HTMLElement = event.currentTarget): LayoutPosition {
  const bounds = target.getBoundingClientRect();
  if (bounds.width <= 0 || bounds.height <= 0) {
    return { adva: '', x: 0.5, y: 0.5 };
  }
  return {
    adva: '',
    x: clampCoordinate((event.clientX - bounds.left) / bounds.width),
    y: clampCoordinate((event.clientY - bounds.top) / bounds.height),
  };
}

function nodeCssCoordinate(value: number): string {
  return `clamp(${FLOORPLAN_NODE_EDGE_INSET_PX}px, ${value * 100}%, calc(100% - ${FLOORPLAN_NODE_EDGE_INSET_PX}px))`;
}

function markerLabelClasses(x: number, y: number): string {
  return `floorplan-marker-label marker-${x < 0.5 ? 'east' : 'west'} marker-${y < 0.5 ? 'south' : 'north'}`;
}

function dataBase64(file: File, signal?: AbortSignal): Promise<string> {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    const cancel = () => reader.abort();
    const cleanup = () => signal?.removeEventListener('abort', cancel);
    reader.onerror = () => {
      cleanup();
      reject(new Error('The selected floorplan could not be read.'));
    };
    reader.onabort = () => {
      cleanup();
      reject(new Error('The selected floorplan read was cancelled.'));
    };
    reader.onload = () => {
      cleanup();
      if (typeof reader.result !== 'string') {
        reject(new Error('The selected floorplan did not produce base64 data.'));
        return;
      }
      const match = /^data:[^;]+;base64,([A-Za-z0-9+/]*={0,2})$/.exec(reader.result);
      if (!match || match[1].length % 4 !== 0) {
        reject(new Error('The selected floorplan is not canonical base64 data.'));
        return;
      }
      resolve(match[1]);
    };
    if (signal?.aborted) {
      reject(new Error('The selected floorplan read was cancelled.'));
      return;
    }
    signal?.addEventListener('abort', cancel, { once: true });
    reader.readAsDataURL(file);
  });
}

export function FloorplanPanel({
  devices,
  healthEpoch,
  currentEpoch,
  healthCurrentCursor,
  rootRecords,
  now,
  healthAuthoritative = true,
  gttRefreshIntervalMs = GTT_REFRESH_INTERVAL_MS,
  gttRequestTimeoutMs = GTT_REQUEST_TIMEOUT_MS,
  gttConfirmationTimeoutMs = GTT_CONFIRMATION_TIMEOUT_MS,
  layoutReadTimeoutMs = LAYOUT_READ_TIMEOUT_MS,
  layoutMutationTimeoutMs = LAYOUT_MUTATION_TIMEOUT_MS,
  incidentViews = [],
  onLayoutChange,
}: FloorplanPanelProps) {
  const topology = gatewayTopology(devices, healthEpoch, currentEpoch, healthCurrentCursor, rootRecords, healthAuthoritative);
  const gateway = gatewayDevice(devices);
  const currentHealthIsAuthoritative = healthAuthoritative && healthEpoch === currentEpoch;
  const canAuthorizeGtt = currentHealthIsAuthoritative && gateway?.connected === true;
  const gttAuthority = {
    canAuthorize: canAuthorizeGtt,
    epoch: currentEpoch,
    generation: gateway?.gtt?.generation ?? 0,
  };
  const bootstrapKey = canAuthorizeGtt && !topology.available
    ? `${currentEpoch}:${gttAuthority.generation}:${topology.issue}`
    : null;
  const [layoutState, setLayoutState] = useState<LayoutState>(initialLayoutState);
  const [selectedForPlacement, setSelectedForPlacement] = useState<string | null>(null);
  const [draftPositions, setDraftPositions] = useState<LayoutPosition[] | null>(null);
  const [gttPending, setGttPending] = useState(false);
  const [gttMessage, setGttMessage] = useState('');
  const layoutRef = useRef<LayoutReadyResponse | null>(null);
  const savingRef = useRef(false);
  const dragRef = useRef<DragState | null>(null);
  const draftPositionsRef = useRef<LayoutPosition[] | null>(null);
  const contentRef = useRef<HTMLDivElement | null>(null);
  const headingRef = useRef<HTMLHeadingElement | null>(null);
  const uploadInputRef = useRef<HTMLInputElement | null>(null);
  const topologyRef = useRef(topology);
  topologyRef.current = topology;
  const gttRequestRef = useRef<ActiveGttRequest | null>(null);
  const gttAuthorityRef = useRef(gttAuthority);
  gttAuthorityRef.current = gttAuthority;
  const gttBootstrapKeyRef = useRef<string | null>(null);
  const layoutReadRef = useRef<ActiveLayoutRead | null>(null);
  const layoutReadId = useRef(0);
  const layoutMutationId = useRef(0);
  const layoutMutationRef = useRef<ActiveLayoutMutation | null>(null);
  const [layoutSynchronized, setLayoutSynchronized] = useState(false);
  const pendingFocusRef = useRef<HTMLElement | null>(null);
  const focusTimerRef = useRef<number | null>(null);

  const setReadyLayout = useCallback((layout: LayoutReadyResponse | null) => {
    layoutRef.current = layout;
    setLayoutState((current) => ({ ...current, layout }));
  }, []);

  const commitReadyLayout = useCallback((layout: LayoutReadyResponse, message: string) => {
    layoutRef.current = layout;
    setLayoutState({ phase: 'ready', layout, message });
    onLayoutChange?.(layout);
  }, [onLayoutChange]);

  const finishLayoutRead = useCallback((read: ActiveLayoutRead): boolean => {
    if (layoutReadRef.current !== read) return false;
    window.clearTimeout(read.timer);
    layoutReadRef.current = null;
    return true;
  }, []);

  const cancelLayoutRead = useCallback(() => {
    const read = layoutReadRef.current;
    if (!read) return;
    window.clearTimeout(read.timer);
    layoutReadRef.current = null;
    read.controller.abort();
  }, []);

  const loadLayout = useCallback(async (message = 'Floorplan loaded.', loadingMessage?: string) => {
    cancelLayoutRead();
    const controller = new AbortController();
    const read: ActiveLayoutRead = {
      id: ++layoutReadId.current,
      controller,
      timer: 0,
      mutationId: layoutMutationId.current,
      timedOut: false,
    };
    layoutReadRef.current = read;
    setLayoutSynchronized(false);
    setLayoutState((current) => ({
      ...current,
      phase: 'loading',
      message: loadingMessage ?? (current.layout ? 'Refreshing floorplan.' : 'Loading floorplan.'),
    }));
    read.timer = window.setTimeout(() => {
      if (layoutReadRef.current !== read) return;
      read.timedOut = true;
      controller.abort();
      finishLayoutRead(read);
      setLayoutSynchronized(false);
      setLayoutState((current) => ({
        phase: 'error',
        layout: current.layout,
        message: 'Floorplan request timed out. Retry floorplan.',
      }));
    }, layoutReadTimeoutMs);
    try {
      const response = await getLayout(controller.signal);
      if (controller.signal.aborted || !finishLayoutRead(read) || layoutMutationId.current !== read.mutationId) return;
      const layout = readyLayout(response);
      if (!layout) {
        layoutRef.current = null;
        onLayoutChange?.(null);
        setLayoutSynchronized(false);
        setLayoutState({ phase: 'error', layout: null, message: 'Floorplan state is corrupt. Recover storage before editing.' });
        return;
      }
      setLayoutSynchronized(true);
      draftPositionsRef.current = null;
      setDraftPositions(null);
      const committedMessage = message === 'Floorplan loaded.' && !layout.floorplan
        ? 'Floorplan coordinates loaded. No image uploaded.'
        : message;
      commitReadyLayout(layout, committedMessage);
    } catch (error) {
      if (controller.signal.aborted || !finishLayoutRead(read) || layoutMutationId.current !== read.mutationId) return;
      setLayoutSynchronized(false);
      setLayoutState((current) => ({
        phase: 'error',
        layout: current.layout,
        message: `Floorplan unavailable: ${messageFor(error)}`,
      }));
    }
  }, [cancelLayoutRead, commitReadyLayout, finishLayoutRead, layoutReadTimeoutMs, onLayoutChange]);

  useEffect(() => {
    void loadLayout();
    return () => {
      cancelLayoutRead();
    };
  }, [cancelLayoutRead, loadLayout]);

  const beginLayoutMutation = useCallback(() => {
    layoutMutationId.current += 1;
    cancelLayoutRead();
  }, [cancelLayoutRead]);

  const recoverLayout = useCallback((layout: LayoutReadyResponse, failure: string, recovery: string) => {
    layoutRef.current = layout;
    setLayoutState({ phase: 'error', layout, message: failure });
    void loadLayout(recovery, `${failure} Refreshing the latest floorplan.`);
  }, [loadLayout]);

  useEffect(() => () => {
    const request = gttRequestRef.current;
    if (request) {
      window.clearTimeout(request.httpTimer);
      if (request.confirmationTimer !== null) window.clearTimeout(request.confirmationTimer);
      request.controller.abort();
    }
    gttRequestRef.current = null;
  }, []);

  useEffect(() => {
    if (!selectedForPlacement) return;
    if (topology.available && topology.topology.entries.some((entry) => entry.adva === selectedForPlacement)) return;
    setSelectedForPlacement(null);
    setLayoutState((current) => ({ ...current, message: 'Placement canceled because the node left the authoritative Gateway roster.' }));
  }, [selectedForPlacement, topology]);

  const queueFocus = useCallback((element: HTMLElement | null) => {
    pendingFocusRef.current = element;
  }, []);

  useEffect(() => {
    if (!pendingFocusRef.current || layoutState.phase === 'saving' || layoutState.phase === 'loading') return undefined;
    if (!layoutSynchronized && layoutState.phase === 'ready') return undefined;
    const focus = (element: HTMLElement | null): boolean => {
      if (!element || !element.isConnected || ('disabled' in element && element.disabled)) return false;
      element.focus();
      return document.activeElement === element;
    };
    const origin = pendingFocusRef.current;
    focusTimerRef.current = window.setTimeout(() => {
      if (focus(origin)) {
        pendingFocusRef.current = null;
        return;
      }
      if (focus(uploadInputRef.current) || focus(headingRef.current)) {
        pendingFocusRef.current = null;
      }
    }, 0);
    return () => {
      if (focusTimerRef.current !== null) window.clearTimeout(focusTimerRef.current);
      focusTimerRef.current = null;
    };
  }, [layoutState.layout, layoutState.phase, layoutSynchronized]);

  useEffect(() => () => {
    if (focusTimerRef.current !== null) window.clearTimeout(focusTimerRef.current);
    focusTimerRef.current = null;
    pendingFocusRef.current = null;
    const mutation = layoutMutationRef.current;
    if (mutation) {
      window.clearTimeout(mutation.timer);
      mutation.controller.abort();
      layoutMutationRef.current = null;
    }
    savingRef.current = false;
  }, []);

  const positionRetryIsAuthorized = useCallback((requiredRosterAdva?: string): boolean => {
    const current = topologyRef.current;
    return current.available
      && (requiredRosterAdva === undefined || current.topology.entries.some((entry) => entry.adva === requiredRosterAdva));
  }, []);

  const isCurrentLayoutMutation = useCallback((mutation: ActiveLayoutMutation): boolean => {
    return layoutMutationRef.current === mutation;
  }, []);

  const finishLayoutMutation = useCallback((mutation: ActiveLayoutMutation): boolean => {
    if (!isCurrentLayoutMutation(mutation)) return false;
    window.clearTimeout(mutation.timer);
    layoutMutationRef.current = null;
    savingRef.current = false;
    return true;
  }, [isCurrentLayoutMutation]);

  const claimLayoutMutation = useCallback((baseline: LayoutReadyResponse, action: string): ActiveLayoutMutation => {
    beginLayoutMutation();
    savingRef.current = true;
    const mutation: ActiveLayoutMutation = {
      id: layoutMutationId.current,
      controller: new AbortController(),
      timer: 0,
      timedOut: false,
    };
    layoutMutationRef.current = mutation;
    mutation.timer = window.setTimeout(() => {
      if (!isCurrentLayoutMutation(mutation)) return;
      mutation.timedOut = true;
      mutation.controller.abort();
      finishLayoutMutation(mutation);
      recoverLayout(baseline, `Floorplan ${action} timed out.`, `Floorplan recovered after ${action} timeout.`);
    }, layoutMutationTimeoutMs);
    return mutation;
  }, [beginLayoutMutation, finishLayoutMutation, isCurrentLayoutMutation, layoutMutationTimeoutMs, recoverLayout]);

  const recoverFailedMutation = useCallback((mutation: ActiveLayoutMutation, layout: LayoutReadyResponse, failure: string, recovery: string) => {
    if (!finishLayoutMutation(mutation)) return;
    recoverLayout(layout, failure, recovery);
  }, [finishLayoutMutation, recoverLayout]);

  const savePositions = useCallback(async (
    transform: PositionTransform,
    success: string,
    focus?: HTMLElement | null,
    requiredRosterAdva?: string,
  ) => {
    const baseline = layoutRef.current;
    if (!baseline || !currentHealthIsAuthoritative || savingRef.current || layoutReadRef.current || layoutMutationRef.current) return;
    if (!positionRetryIsAuthorized(requiredRosterAdva)) {
      if (requiredRosterAdva) {
        setSelectedForPlacement((selected) => selected === requiredRosterAdva ? null : selected);
        setLayoutState((current) => ({ ...current, phase: 'error', message: 'Placement canceled because the node is no longer in the authoritative Gateway roster.' }));
        queueFocus(focus ?? null);
      }
      return;
    }
    const intended = sortedPositions(transform(baseline.positions));
    if (intended.length > 16) {
      setLayoutState((current) => ({ ...current, phase: 'error', message: 'Floorplan position limit reached (16). Unplace a stored position before placing another node.' }));
      queueFocus(focus ?? null);
      return;
    }
    const mutation = claimLayoutMutation(baseline, 'save');
    queueFocus(focus ?? null);
    draftPositionsRef.current = null;
    setDraftPositions(null);
    setReadyLayout({ ...baseline, positions: intended });
    setLayoutState((current) => ({ ...current, phase: 'saving', message: 'Saving floorplan.' }));
    const persist = async (current: LayoutReadyResponse, positions: LayoutPosition[]): Promise<LayoutReadyResponse> => {
      const response = await putLayout(current.revision, positions, mutation.controller.signal);
      const ready = readyLayout(response);
      if (!ready) throw new ApiError('Floorplan state became corrupt.');
      return ready;
    };
    try {
      const saved = await persist(baseline, intended);
      if (!finishLayoutMutation(mutation)) return;
      commitReadyLayout(saved, success);
      queueFocus(focus ?? null);
    } catch (error) {
      if (!isCurrentLayoutMutation(mutation) || mutation.timedOut) return;
      if (error instanceof LayoutConflictError) {
        const current = readyLayout(error.current);
        if (current) {
          if (!positionRetryIsAuthorized(requiredRosterAdva)) {
            recoverFailedMutation(mutation, current, 'Floorplan save was not retried because Gateway roster authority changed.', 'Floorplan recovered after save conflict.');
            queueFocus(focus ?? null);
            return;
          }
          const reapplied = sortedPositions(transform(current.positions));
          if (reapplied.length > 16) {
            recoverFailedMutation(mutation, current, 'Floorplan save could not be reapplied: position capacity reached.', 'Floorplan recovered after save conflict.');
            queueFocus(focus ?? null);
            return;
          }
          setReadyLayout({ ...current, positions: reapplied });
          try {
            const saved = await persist(current, reapplied);
            if (!finishLayoutMutation(mutation)) return;
            commitReadyLayout(saved, `${success} Reapplied after a concurrent update.`);
            queueFocus(focus ?? null);
          } catch (retryError) {
            if (!isCurrentLayoutMutation(mutation) || mutation.timedOut) return;
            const rollback = retryError instanceof LayoutConflictError ? readyLayout(retryError.current) ?? current : current;
            recoverFailedMutation(mutation, rollback, `Floorplan save failed after retry: ${messageFor(retryError)}`, 'Floorplan recovered after save failure.');
            queueFocus(focus ?? null);
          }
        } else {
          recoverFailedMutation(mutation, baseline, 'Floorplan save conflict could not be recovered.', 'Floorplan recovered after conflict.');
          queueFocus(focus ?? null);
        }
      } else {
        recoverFailedMutation(mutation, baseline, `Floorplan save failed: ${messageFor(error)}`, 'Floorplan recovered after save failure.');
        queueFocus(focus ?? null);
      }
    }
  }, [claimLayoutMutation, commitReadyLayout, currentHealthIsAuthoritative, finishLayoutMutation, isCurrentLayoutMutation, positionRetryIsAuthorized, queueFocus, recoverFailedMutation, setReadyLayout]);

  const saveImage = useCallback(async (file: File, focus: HTMLElement | null) => {
    const baseline = layoutRef.current;
    if (!baseline || savingRef.current || layoutReadRef.current || layoutMutationRef.current || !isAcceptedFloorplan(file)) return;
    const mutation = claimLayoutMutation(baseline, 'upload');
    queueFocus(focus);
    setLayoutState((current) => ({ ...current, phase: 'saving', message: 'Uploading floorplan.' }));
    try {
      const encoded = await dataBase64(file, mutation.controller.signal);
      if (!isCurrentLayoutMutation(mutation)) return;
      const persist = async (current: LayoutReadyResponse): Promise<LayoutReadyResponse> => {
        const response = await uploadFloorplan(current.revision, file.type, encoded, mutation.controller.signal);
        const ready = readyLayout(response);
        if (!ready) throw new ApiError('Floorplan state became corrupt.');
        return ready;
      };
      try {
        const saved = await persist(baseline);
        if (!finishLayoutMutation(mutation)) return;
        commitReadyLayout(saved, 'Floorplan updated; node positions retained.');
        queueFocus(focus);
      } catch (error) {
        if (!isCurrentLayoutMutation(mutation) || mutation.timedOut) return;
        if (!(error instanceof LayoutConflictError)) throw error;
        const current = readyLayout(error.current);
        if (!current) throw error;
        setReadyLayout(current);
        try {
          const saved = await persist(current);
          if (!finishLayoutMutation(mutation)) return;
          commitReadyLayout(saved, 'Floorplan updated; node positions retained after a concurrent update.');
          queueFocus(focus);
        } catch (retryError) {
          if (!isCurrentLayoutMutation(mutation) || mutation.timedOut) return;
          const rollback = retryError instanceof LayoutConflictError ? readyLayout(retryError.current) ?? current : current;
          recoverFailedMutation(mutation, rollback, `Floorplan upload failed after retry: ${messageFor(retryError)}`, 'Floorplan recovered after upload failure.');
          queueFocus(focus);
        }
      }
    } catch (error) {
      if (!isCurrentLayoutMutation(mutation) || mutation.timedOut) return;
      recoverFailedMutation(mutation, baseline, `Floorplan upload failed: ${messageFor(error)}`, 'Floorplan recovered after upload failure.');
      queueFocus(focus);
    }
  }, [claimLayoutMutation, commitReadyLayout, finishLayoutMutation, isCurrentLayoutMutation, queueFocus, recoverFailedMutation, setReadyLayout]);

  const removeImage = useCallback(async (focus: HTMLElement | null) => {
    const baseline = layoutRef.current;
    if (!baseline || savingRef.current || layoutReadRef.current || layoutMutationRef.current) return;
    const mutation = claimLayoutMutation(baseline, 'removal');
    queueFocus(focus);
    setLayoutState((current) => ({ ...current, phase: 'saving', message: 'Removing floorplan.' }));
    try {
      const persist = async (current: LayoutReadyResponse): Promise<LayoutReadyResponse> => {
        const response = await removeFloorplan(current.revision, mutation.controller.signal);
        const ready = readyLayout(response);
        if (!ready) throw new ApiError('Floorplan state became corrupt.');
        return ready;
      };
      try {
        const saved = await persist(baseline);
        if (!finishLayoutMutation(mutation)) return;
        commitReadyLayout(saved, 'Floorplan removed; node positions retained.');
        queueFocus(focus);
      } catch (error) {
        if (!isCurrentLayoutMutation(mutation) || mutation.timedOut) return;
        if (!(error instanceof LayoutConflictError)) throw error;
        const current = readyLayout(error.current);
        if (!current) throw error;
        setReadyLayout(current);
        try {
          const saved = await persist(current);
          if (!finishLayoutMutation(mutation)) return;
          commitReadyLayout(saved, 'Floorplan removed; node positions retained after a concurrent update.');
          queueFocus(focus);
        } catch (retryError) {
          if (!isCurrentLayoutMutation(mutation) || mutation.timedOut) return;
          const rollback = retryError instanceof LayoutConflictError ? readyLayout(retryError.current) ?? current : current;
          recoverFailedMutation(mutation, rollback, `Floorplan removal failed after retry: ${messageFor(retryError)}`, 'Floorplan recovered after removal failure.');
          queueFocus(focus);
        }
      }
    } catch (error) {
      if (!isCurrentLayoutMutation(mutation) || mutation.timedOut) return;
      recoverFailedMutation(mutation, baseline, `Floorplan removal failed: ${messageFor(error)}`, 'Floorplan recovered after removal failure.');
      queueFocus(focus);
    }
  }, [claimLayoutMutation, commitReadyLayout, finishLayoutMutation, isCurrentLayoutMutation, queueFocus, recoverFailedMutation, setReadyLayout]);

  const completeGttRequest = useCallback((request: ActiveGttRequest): boolean => {
    const authority = gttAuthorityRef.current;
    if (gttRequestRef.current !== request
      || !request.httpAccepted
      || !authority.canAuthorize
      || request.epoch !== authority.epoch
      || authority.generation <= request.generation) {
      return false;
    }
    window.clearTimeout(request.httpTimer);
    if (request.confirmationTimer !== null) window.clearTimeout(request.confirmationTimer);
    gttRequestRef.current = null;
    setGttPending(false);
    setGttMessage('Gateway GTT updated.');
    return true;
  }, []);

  const requestGtt = useCallback(async () => {
    const authority = gttAuthorityRef.current;
    if (!authority.canAuthorize || gttRequestRef.current) return;
    const request: ActiveGttRequest = {
      generation: authority.generation,
      epoch: authority.epoch,
      controller: new AbortController(),
      httpTimer: 0,
      confirmationTimer: null,
      httpTimedOut: false,
      httpAccepted: false,
    };
    gttRequestRef.current = request;
    setGttPending(true);
    setGttMessage('Refreshing Gateway GTT.');
    request.httpTimer = window.setTimeout(() => {
      if (gttRequestRef.current !== request) return;
      request.httpTimedOut = true;
      request.controller.abort();
      gttRequestRef.current = null;
      setGttPending(false);
      setGttMessage('Gateway GTT request timed out. Retry Gateway.');
    }, gttRequestTimeoutMs);
    try {
      await postGtt(0, request.controller.signal);
      if (gttRequestRef.current !== request) return;
      window.clearTimeout(request.httpTimer);
      request.httpAccepted = true;
      if (completeGttRequest(request)) return;
      setGttMessage('Waiting for Gateway GTT update.');
      request.confirmationTimer = window.setTimeout(() => {
        if (gttRequestRef.current !== request) return;
        gttRequestRef.current = null;
        setGttPending(false);
        setGttMessage('Gateway GTT refresh timed out. Retry Gateway.');
      }, gttConfirmationTimeoutMs);
    } catch {
      if (gttRequestRef.current !== request || request.httpTimedOut) return;
      window.clearTimeout(request.httpTimer);
      if (request.confirmationTimer !== null) window.clearTimeout(request.confirmationTimer);
      gttRequestRef.current = null;
      setGttPending(false);
      setGttMessage('Gateway GTT request could not be completed. Retry Gateway.');
    }
  }, [completeGttRequest, gttConfirmationTimeoutMs, gttRequestTimeoutMs]);

  useEffect(() => {
    const request = gttRequestRef.current;
    if (!request || request.epoch === currentEpoch) return;
    window.clearTimeout(request.httpTimer);
    if (request.confirmationTimer !== null) window.clearTimeout(request.confirmationTimer);
    request.controller.abort();
    gttRequestRef.current = null;
    setGttPending(false);
    setGttMessage('Gateway GTT refresh was reset with the bridge. Await current Gateway health before retrying.');
  }, [currentEpoch]);

  useEffect(() => {
    const request = gttRequestRef.current;
    if (canAuthorizeGtt || !request) return;
    window.clearTimeout(request.httpTimer);
    if (request.confirmationTimer !== null) window.clearTimeout(request.confirmationTimer);
    request.controller.abort();
    gttRequestRef.current = null;
    setGttPending(false);
    setGttMessage('Gateway GTT refresh was canceled because current Gateway health is unavailable.');
  }, [canAuthorizeGtt]);

  useEffect(() => {
    const request = gttRequestRef.current;
    if (request) completeGttRequest(request);
  }, [completeGttRequest, canAuthorizeGtt, currentEpoch, gttAuthority.generation]);

  useEffect(() => {
    if (!bootstrapKey) {
      gttBootstrapKeyRef.current = null;
      return;
    }
    if (gttBootstrapKeyRef.current === bootstrapKey) return;
    gttBootstrapKeyRef.current = bootstrapKey;
    void requestGtt();
  }, [bootstrapKey, requestGtt]);

  useEffect(() => {
    if (!canAuthorizeGtt) return undefined;
    const timer = window.setInterval(() => { void requestGtt(); }, gttRefreshIntervalMs);
    return () => window.clearInterval(timer);
  }, [canAuthorizeGtt, currentEpoch, gttRefreshIntervalMs, requestGtt]);

  const layout = layoutState.layout;
  const positions = draftPositions ?? layout?.positions ?? [];
  const positioned = new Set(positions.map((position) => position.adva));
  const entries = topology.available ? topology.topology.entries : [];
  const local = topology.available ? topology.topology.gtt.local : '';
  const unpositioned = entries.filter((entry) => !positioned.has(entry.adva));
  const storedOutsideRoster = positions.filter((position) => !entries.some((entry) => entry.adva === position.adva));
  const isSaving = layoutState.phase === 'saving';
  const isEditing = !layoutSynchronized || isSaving || layoutState.phase === 'loading';
  const canEditPositions = !isEditing && currentHealthIsAuthoritative && topology.available;
  const canRequestGtt = canAuthorizeGtt;
  const recentIncidentViews = incidentViews.slice(0, 10);

  const finishDrag = (element: HTMLElement | null) => {
    const drag = dragRef.current;
    if (!drag) return;
    dragRef.current = null;
    const draft = draftPositionsRef.current;
    const position = draft && positionFor(draft, drag.adva);
    if (position) {
      void savePositions((current) => setPosition(current, position), 'Node position saved.', element, drag.adva);
    }
  };

  return (
    <section className="panel floorplan-panel" aria-labelledby="floorplan-heading">
      <div className="section-heading">
        <div>
          <p className="eyebrow">Gateway GTT floorplan</p>
          <h2 ref={headingRef} id="floorplan-heading" tabIndex={-1}>Floorplan</h2>
        </div>
        <div className="floorplan-actions">
          <label className="secondary file-action">
            <span>Upload image</span>
            <input
              ref={uploadInputRef}
              type="file"
              accept="image/png,image/jpeg,image/webp"
              disabled={!layout || isEditing}
              onChange={(event) => {
                const file = event.currentTarget.files?.[0];
                event.currentTarget.value = '';
                if (!file) return;
                if (!isAcceptedFloorplan(file)) {
                  setLayoutState((current) => ({ ...current, phase: 'error', message: 'Choose a PNG, JPEG, or WebP image no larger than 5 MiB.' }));
                  return;
                }
                void saveImage(file, event.currentTarget);
              }}
            />
          </label>
          {layout?.floorplan && <button type="button" className="secondary" disabled={isEditing} onClick={(event) => void removeImage(event.currentTarget)}>Remove image</button>}
        </div>
      </div>
      <p className={`floorplan-status ${layoutState.phase}`} role="status" aria-live="polite">{layoutState.message}</p>
      {!layoutSynchronized && layoutState.phase === 'error' && <button type="button" className="secondary" onClick={() => void loadLayout('Floorplan recovered.')}>Retry floorplan</button>}
      {gttMessage && <p className="gtt-status" role="status" aria-live="polite">{gttMessage}</p>}
      {!topology.available ? (
        <div className="topology-unavailable" role="alert">
          <p>{topologyMessage(topology.issue)}</p>
          {canRequestGtt && <button type="button" className="secondary" onClick={() => void requestGtt()} disabled={gttPending}>Request Gateway GTT</button>}
        </div>
      ) : (
        <div className="floorplan-toolbar">
          <span className={`status-chip ${gttPending ? 'warning' : 'good'}`}>{gttPending ? 'Refreshing GTT' : 'GTT ready'}</span>
          <span className="floorplan-freshness">Updated {formatAge(Math.max(0, now - topology.topology.gtt.completed_at_ms))}</span>
          <button type="button" className="secondary" disabled={gttPending || isEditing} onClick={() => void requestGtt()}>{gttPending ? 'Refreshing GTT' : 'Refresh GTT'}</button>
        </div>
      )}
      {layout ? (
        <div
          ref={contentRef}
          className={`floorplan-content${selectedForPlacement && topology.available ? ' placement-active' : ''}`}
          style={{ aspectRatio: floorplanAspect(layout) }}
          onClick={(event) => {
            if (!selectedForPlacement || !canEditPositions) return;
            const point = pointAt(event);
            const selected = selectedForPlacement;
            setSelectedForPlacement(null);
            void savePositions((current) => setPosition(current, { ...point, adva: selected }), 'Node placed on floorplan.', event.currentTarget, selected);
          }}
          aria-label={selectedForPlacement && topology.available ? 'Floorplan placement target' : 'Floorplan'}
        >
          {layout.floorplan && <img src={layout.floorplan.url} alt="Uploaded floorplan" draggable={false} />}
          {entries.map((entry) => {
            const position = positionFor(positions, entry.adva);
            if (!position) return null;
            const name = gatewayEntryLabel(entry, local);
            const instructions = `Use arrow keys to move ${name} by 0.01. Hold Shift for 0.05. Drag to move, then release to save.`;
            return (
              <button
                key={entry.adva}
                type="button"
                className={`floorplan-node freshness-${entry.freshness}`}
                style={{ left: nodeCssCoordinate(position.x), top: nodeCssCoordinate(position.y) }}
                disabled={!canEditPositions}
                aria-label={`${name}, positioned at x ${position.x.toFixed(2)}, y ${position.y.toFixed(2)}, ${freshnessText(entry)}, ${departureText(entry)}`}
                aria-describedby={`node-instructions-${entry.adva}`}
                onPointerDown={(event) => {
                  if (!canEditPositions) return;
                  event.stopPropagation();
                  event.currentTarget.focus();
                  event.currentTarget.setPointerCapture?.(event.pointerId);
                  dragRef.current = { adva: entry.adva, pointerId: event.pointerId };
                  draftPositionsRef.current = layout.positions;
                  setDraftPositions(layout.positions);
                }}
                onPointerMove={(event) => {
                  const drag = dragRef.current;
                  if (!drag || drag.pointerId !== event.pointerId) return;
                  event.stopPropagation();
                  const content = contentRef.current;
                  if (!content) return;
                  const point = pointAt(event, content);
                  setDraftPositions((current) => {
                    const next = setPosition(current ?? layout.positions, { ...point, adva: drag.adva });
                    draftPositionsRef.current = next;
                    return next;
                  });
                }}
                onPointerUp={(event) => {
                  if (dragRef.current?.pointerId !== event.pointerId) return;
                  event.stopPropagation();
                  if (event.currentTarget.hasPointerCapture?.(event.pointerId)) event.currentTarget.releasePointerCapture?.(event.pointerId);
                  finishDrag(event.currentTarget);
                }}
                onPointerCancel={(event) => {
                  if (dragRef.current?.pointerId !== event.pointerId) return;
                  if (event.currentTarget.hasPointerCapture?.(event.pointerId)) event.currentTarget.releasePointerCapture?.(event.pointerId);
                  draftPositionsRef.current = null;
                  setDraftPositions(null);
                  dragRef.current = null;
                }}
                onKeyDown={(event) => {
                  const delta = event.shiftKey ? LARGE_KEYBOARD_STEP : SMALL_KEYBOARD_STEP;
                  const current = positionFor(draftPositionsRef.current ?? layout.positions, entry.adva);
                  if (!current) return;
                  let next: LayoutPosition | null = null;
                  if (event.key === 'ArrowLeft') next = { ...current, x: clampCoordinate(current.x - delta) };
                  if (event.key === 'ArrowRight') next = { ...current, x: clampCoordinate(current.x + delta) };
                  if (event.key === 'ArrowUp') next = { ...current, y: clampCoordinate(current.y - delta) };
                  if (event.key === 'ArrowDown') next = { ...current, y: clampCoordinate(current.y + delta) };
                  if (!next) return;
                  event.preventDefault();
                  setDraftPositions((currentPositions) => {
                    const updated = setPosition(currentPositions ?? layout.positions, next);
                    draftPositionsRef.current = updated;
                    return updated;
                  });
                }}
                onKeyUp={(event) => {
                  if (!['ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown'].includes(event.key)) return;
                  const next = positionFor(draftPositionsRef.current ?? layout.positions, entry.adva);
                  if (next) void savePositions((current) => setPosition(current, next), 'Node position saved.', event.currentTarget, entry.adva);
                }}
              >
                {name === 'Gateway' ? 'G' : entry.adva.slice(-4)}
                <span id={`node-instructions-${entry.adva}`} className="sr-only">{instructions}</span>
              </button>
            );
          })}
          {recentIncidentViews.filter((view) => view.status === 'ballpark').map((view) => (
            <span
              key={view.logicalEvent.key}
              className="floorplan-marker"
              role="img"
              style={{ left: `${view.x * 100}%`, top: `${view.y * 100}%` }}
              aria-label={`Wearable ${view.logicalEvent.record.wearable}, packet ${view.logicalEvent.record.packet}: Ballpark at normalized x ${view.x.toFixed(2)}, y ${view.y.toFixed(2)}`}
            >
              <span className="floorplan-marker-point" aria-hidden="true" />
              <span className={markerLabelClasses(view.x, view.y)} aria-hidden="true">W{view.logicalEvent.record.wearable}</span>
            </span>
          ))}
        </div>
      ) : <p className="empty">Floorplan is loading.</p>}
      <div className="table-wrap">
        <table className="estimate-table">
          <caption>Recent incident location states</caption>
          <thead><tr><th scope="col">Incident</th><th scope="col">Location</th><th scope="col">Contributors</th><th scope="col">Geometry</th><th scope="col">Normalized spread</th><th scope="col">Normalized coordinates</th></tr></thead>
          <tbody>
            {recentIncidentViews.length === 0 ? <tr><td colSpan={6} className="empty">No recent incident location states.</td></tr> : recentIncidentViews.map((view) => (
              <tr key={view.logicalEvent.key}>
                <th scope="row">Wearable {view.logicalEvent.record.wearable} <code>{view.logicalEvent.record.packet}</code></th>
                <td data-label="Location"><span>{view.status === 'collecting' ? 'Collecting' : view.status === 'insufficient' ? 'Insufficient' : 'Ballpark'}</span></td>
                <td data-label="Contributors">{view.status === 'collecting' ? '—' : view.status === 'insufficient' ? `${view.contributorCount} of 3 required` : view.contributorCount}</td>
                <td data-label="Geometry">{view.status === 'ballpark' ? (view.geometryWarning ? 'Warning' : 'No warning') : '—'}</td>
                <td data-label="Normalized spread">{view.status === 'ballpark' ? (view.normalizedSpread === null ? 'Not available' : view.normalizedSpread.toFixed(2)) : '—'}</td>
                <td data-label="Normalized coordinates">{view.status === 'ballpark' ? `${view.x.toFixed(2)}, ${view.y.toFixed(2)}` : '—'}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
      {topology.available && (
        <>
          <section className="staging-roster" aria-labelledby="staging-heading">
            <div className="section-heading">
              <h3 id="staging-heading">Unpositioned nodes <span className="count">{unpositioned.length}</span> <span className="count">{positions.length}/16 stored</span></h3>
              {selectedForPlacement && <button type="button" className="secondary" disabled={!canEditPositions} onClick={() => setSelectedForPlacement(null)}>Cancel placement</button>}
            </div>
            {unpositioned.length === 0 ? <p className="empty">All current Gateway GTT entries are positioned.</p> : (
              <ul className="staging-list">
                {unpositioned.map((entry) => {
                  const name = gatewayEntryLabel(entry, local);
                  return (
                    <li key={entry.adva}>
                      <strong>{name === 'Gateway' ? name : <code>{name}</code>}</strong>
                      <span>{freshnessText(entry)} · {departureText(entry)}</span>
                      <button type="button" className="secondary" disabled={!canEditPositions} onClick={() => setSelectedForPlacement(entry.adva)}>Place on map</button>
                      <button type="button" className="secondary" disabled={!canEditPositions} onClick={(event) => void savePositions((current) => setPosition(current, { adva: entry.adva, x: 0.5, y: 0.5 }), 'Node placed at center.', event.currentTarget, entry.adva)}>Place at center</button>
                    </li>
                  );
                })}
              </ul>
            )}
          </section>
          <div className="table-wrap">
            <table className="node-table">
              <caption>Gateway GTT nodes and stored floorplan coordinates</caption>
              <thead><tr><th scope="col">Node</th><th scope="col">Map state</th><th scope="col">Coordinates</th><th scope="col">Freshness</th><th scope="col">Departure</th><th scope="col">Actions</th></tr></thead>
              <tbody>
                {entries.map((entry) => {
                  const position = positionFor(positions, entry.adva);
                  const name = gatewayEntryLabel(entry, local);
                  return (
                    <tr key={entry.adva}>
                      <th scope="row">{name === 'Gateway' ? 'Gateway' : <code>{name}</code>}</th>
                      <td data-label="Map state">{position ? 'Positioned' : 'Unpositioned'}</td>
                      <td data-label="Coordinates">{coordinateText(position)}</td>
                      <td data-label="Freshness">{freshnessText(entry)}</td>
                      <td data-label="Departure">{departureText(entry)}</td>
                      <td data-label="Actions">
                         {position ? <button type="button" className="secondary" disabled={!canEditPositions} onClick={(event) => void savePositions((current) => removePosition(current, entry.adva), 'Node unplaced.', event.currentTarget, entry.adva)}>Unplace</button>
                          : <button type="button" className="secondary" disabled={!canEditPositions} onClick={(event) => void savePositions((current) => setPosition(current, { adva: entry.adva, x: 0.5, y: 0.5 }), 'Node placed at center.', event.currentTarget, entry.adva)}>Place at center</button>}
                      </td>
                    </tr>
                  );
                })}
                {storedOutsideRoster.map((position) => (
                  <tr key={position.adva}>
                    <th scope="row"><code>{position.adva}</code></th>
                    <td data-label="Map state">Stored, not in current GTT</td>
                    <td data-label="Coordinates">{coordinateText(position)}</td>
                    <td data-label="Freshness">Not in current GTT</td>
                    <td data-label="Departure">Unknown</td>
                    <td data-label="Actions"><button type="button" className="secondary" disabled={!canEditPositions} onClick={(event) => void savePositions((current) => removePosition(current, position.adva), 'Stored node unplaced.', event.currentTarget)}>Unplace</button></td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
          {layout && layout.positions.length > 0 && <button type="button" className="secondary clear-positions" disabled={!canEditPositions} onClick={(event) => {
            const count = layout.positions.length;
            if (!window.confirm(`Clear all ${count} stored node position${count === 1 ? '' : 's'}? This removes their floorplan coordinates.`)) {
              event.currentTarget.focus();
              return;
            }
            void savePositions(() => [], 'All node positions cleared.', event.currentTarget);
          }}>Clear all positions</button>}
        </>
      )}
    </section>
  );
}
