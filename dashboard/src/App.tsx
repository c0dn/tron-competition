import { useEffect, useReducer, useRef, useState } from 'react';
import { fetchEvents, fetchHealth, postRoot } from './lib/api';
import { Header } from './components/Header';
import { IncidentFeed } from './components/IncidentFeed';
import { RootControlPanel } from './components/RootControlPanel';
import { TopologyPanel } from './components/TopologyPanel';
import { WearablePanel } from './components/WearablePanel';
import { bridgeStatus, initialState, reducer } from './state/store';
import { rootRequestBaseline } from './state/devices';

export const POLL_INTERVAL_MS = 4_000;
export const ROOT_REQUEST_TIMEOUT_MS = 5_000;
export const ROOT_CONFIRMATION_TIMEOUT_MS = 10_000;

interface ActiveRootRequest {
  id: number;
  controller: AbortController;
  httpTimer: number;
  confirmationTimer: number;
  httpTimedOut: boolean;
  confirmationTimedOut: boolean;
}

interface AppProps {
  pollIntervalMs?: number;
  rootRequestTimeoutMs?: number;
  rootConfirmationTimeoutMs?: number;
}

function messageFor(error: unknown): string {
  return error instanceof Error ? error.message : 'Unknown bridge error.';
}

export default function App({
  pollIntervalMs = POLL_INTERVAL_MS,
  rootRequestTimeoutMs = ROOT_REQUEST_TIMEOUT_MS,
  rootConfirmationTimeoutMs = ROOT_CONFIRMATION_TIMEOUT_MS,
}: AppProps) {
  const [state, dispatch] = useReducer(reducer, initialState);
  const [now, setNow] = useState(() => Date.now());
  const eventCursor = useRef(0);
  const eventEpoch = useRef(0);
  const rootRequestId = useRef(0);
  const rootRequests = useRef(new Map<number, ActiveRootRequest>());

  useEffect(() => {
    const timer = window.setInterval(() => setNow(Date.now()), 1_000);
    return () => window.clearInterval(timer);
  }, []);

  useEffect(() => {
    const controller = new AbortController();
    let timer: number | undefined;
    let stopped = false;

    const drainEvents = async () => {
      let requestedAfter = eventCursor.current;
      while (!stopped) {
        const page = await fetchEvents(requestedAfter, controller.signal);
        if (page.current_cursor < requestedAfter) {
          eventCursor.current = 0;
          eventEpoch.current += 1;
          dispatch({ type: 'epochReset' });
          requestedAfter = 0;
          continue;
        }
        dispatch({ type: 'eventsReceived', page, receivedAt: Date.now(), epoch: eventEpoch.current });
        const finalRecordCursor = page.events.at(-1)?.cursor;
        if (page.events.length === 100 && finalRecordCursor !== undefined && page.current_cursor > finalRecordCursor) {
          eventCursor.current = finalRecordCursor;
          requestedAfter = finalRecordCursor;
          continue;
        }
        eventCursor.current = page.current_cursor;
        return;
      }
    };

    const poll = async () => {
      const requestEpoch = eventEpoch.current;
      const events = drainEvents().catch((error: unknown) => {
        if (!controller.signal.aborted) dispatch({ type: 'requestFailed', source: 'events', message: messageFor(error) });
      });
      const health = fetchHealth(controller.signal)
        .then((response) => dispatch({ type: 'healthReceived', health: response, receivedAt: Date.now(), epoch: requestEpoch }))
        .catch((error: unknown) => {
          if (!controller.signal.aborted) dispatch({ type: 'requestFailed', source: 'health', message: messageFor(error) });
        });
      await Promise.all([events, health]);
      if (!stopped) timer = window.setTimeout(poll, pollIntervalMs);
    };

    void poll();
    return () => {
      stopped = true;
      controller.abort();
      if (timer !== undefined) window.clearTimeout(timer);
    };
  }, [pollIntervalMs]);

  useEffect(() => {
    for (const [device, request] of rootRequests.current) {
      const pending = state.pendingRoot[device];
      if (pending?.requestId === request.id) continue;
      window.clearTimeout(request.httpTimer);
      window.clearTimeout(request.confirmationTimer);
      request.controller.abort();
      rootRequests.current.delete(device);
    }
  }, [state.pendingRoot]);

  useEffect(() => () => {
    for (const request of rootRequests.current.values()) {
      window.clearTimeout(request.httpTimer);
      window.clearTimeout(request.confirmationTimer);
      request.controller.abort();
    }
    rootRequests.current.clear();
  }, []);

  const requestRoot = async (device: number, active: boolean) => {
    if (rootRequests.current.has(device)) return;
    const requestId = ++rootRequestId.current;
    const controller = new AbortController();
    const request: ActiveRootRequest = {
      id: requestId,
      controller,
      httpTimer: 0,
      confirmationTimer: 0,
      httpTimedOut: false,
      confirmationTimedOut: false,
    };
    request.httpTimer = window.setTimeout(() => {
      if (rootRequests.current.get(device)?.id !== requestId) return;
      request.httpTimedOut = true;
      controller.abort();
      dispatch({ type: 'rootFailed', device, requestId, message: 'Request timed out; retry this device.' });
    }, rootRequestTimeoutMs);
    request.confirmationTimer = window.setTimeout(() => {
      if (rootRequests.current.get(device)?.id !== requestId) return;
      request.confirmationTimedOut = true;
      controller.abort();
      dispatch({ type: 'rootConfirmationTimedOut', device, requestId });
    }, rootConfirmationTimeoutMs);
    rootRequests.current.set(device, request);
    const healthDevice = state.health?.devices.find((candidate) => candidate.device === device);
    dispatch({
      type: 'rootPending',
      device,
      active,
      requestId,
      baselineCursor: rootRequestBaseline(
        state.eventCursor,
        device,
        healthDevice,
        state.healthEpoch,
        state.epoch,
        state.health?.current_cursor ?? null,
        state.rootRecords,
      ),
      baselineEpoch: state.epoch,
    });
    try {
      const response = await postRoot(device, active, controller.signal);
      if (rootRequests.current.get(device)?.id !== requestId) return;
      window.clearTimeout(request.httpTimer);
      dispatch({ type: 'rootAccepted', device: response.device, command: response.command, requestId });
    } catch (error) {
      if (request.httpTimedOut || request.confirmationTimedOut || rootRequests.current.get(device)?.id !== requestId) return;
      window.clearTimeout(request.httpTimer);
      window.clearTimeout(request.confirmationTimer);
      dispatch({
        type: 'rootFailed',
        device,
        requestId,
        message: messageFor(error),
      });
    }
  };

  const devices = state.health?.devices ?? [];
  const events = state.feedKeys
    .map((key) => state.logicalEvents[key])
    .filter((event): event is NonNullable<typeof event> => event !== undefined);

  return (
    <>
      <Header status={bridgeStatus(state, now)} />
      <main>
        <div className="primary-column">
          {state.lastGap && (
            <p className="notice warning gap-notice" role="alert">
              Event history gap detected. The bridge resumed from cursor {state.lastGap.oldest_cursor} and retained recovered records through {state.lastGap.current_cursor}.
            </p>
          )}
          <RootControlPanel
            devices={devices}
            healthEpoch={state.healthEpoch}
            currentEpoch={state.epoch}
            healthCurrentCursor={state.health?.current_cursor ?? null}
            rootRecords={state.rootRecords}
            commandRecords={state.commandRecords}
            pendingRoot={state.pendingRoot}
            announcement={state.announcement}
            healthError={state.healthRequest.error}
            onSetRoot={requestRoot}
          />
          <TopologyPanel
            devices={devices}
            healthEpoch={state.healthEpoch}
            currentEpoch={state.epoch}
            healthCurrentCursor={state.health?.current_cursor ?? null}
            rootRecords={state.rootRecords}
            now={now}
          />
          <WearablePanel wearables={state.wearables} now={now} />
        </div>
        <IncidentFeed
          events={events}
          conflictCount={state.conflictCount}
          loading={state.eventsRequest.phase === 'loading'}
          error={state.eventsRequest.error}
        />
      </main>
    </>
  );
}
