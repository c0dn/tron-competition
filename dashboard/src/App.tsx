import { useEffect, useReducer, useRef, useState } from 'react';
import { fetchEvents, fetchHealth, postRoot } from './lib/api';
import { Header } from './components/Header';
import { IncidentFeed } from './components/IncidentFeed';
import { RootControlPanel } from './components/RootControlPanel';
import { WearablePanel } from './components/WearablePanel';
import { bridgeStatus, initialState, reducer } from './state/store';

export const POLL_INTERVAL_MS = 4_000;
export const ROOT_REQUEST_TIMEOUT_MS = 5_000;

interface AppProps {
  pollIntervalMs?: number;
  rootRequestTimeoutMs?: number;
}

function messageFor(error: unknown): string {
  return error instanceof Error ? error.message : 'Unknown bridge error.';
}

export default function App({
  pollIntervalMs = POLL_INTERVAL_MS,
  rootRequestTimeoutMs = ROOT_REQUEST_TIMEOUT_MS,
}: AppProps) {
  const [state, dispatch] = useReducer(reducer, initialState);
  const [now, setNow] = useState(() => Date.now());
  const eventCursor = useRef(0);
  const eventEpoch = useRef(0);

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

  const requestRoot = async (device: number, active: boolean) => {
    dispatch({ type: 'rootPending', device, active });
    const controller = new AbortController();
    let timedOut = false;
    const timeout = window.setTimeout(() => {
      timedOut = true;
      controller.abort();
    }, rootRequestTimeoutMs);
    try {
      const response = await postRoot(device, active, controller.signal);
      dispatch({ type: 'rootAccepted', device: response.device, command: response.command });
    } catch (error) {
      dispatch({
        type: 'rootFailed',
        device,
        message: timedOut ? 'Request timed out; retry this device.' : messageFor(error),
      });
    } finally {
      window.clearTimeout(timeout);
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
            healthCurrentCursor={state.health?.current_cursor}
            healthEpoch={state.healthEpoch}
            currentEpoch={state.epoch}
            rootRecords={state.rootRecords}
            commandRecords={state.commandRecords}
            pendingRoot={state.pendingRoot}
            announcement={state.announcement}
            healthError={state.healthRequest.error}
            onSetRoot={requestRoot}
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
