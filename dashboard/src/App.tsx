import { useReducer, useState, useEffect, useCallback } from 'react';
import { reducer, initialState } from './state/store';
import { useMqtt } from './lib/useMqtt';
import { Header } from './components/Header';
import { ConnectionDrawer } from './components/ConnectionDrawer';
import { WearablePanel } from './components/WearablePanel';
import { NodePanel } from './components/NodePanel';
import { IncidentFeed } from './components/IncidentFeed';

function defaultBrokerUrl(): string {
  const host = location.hostname || 'localhost';
  return `ws://${host}:9001`;
}

export default function App() {
  const [state, dispatch] = useReducer(reducer, initialState);
  const [url, setUrl] = useState(() => localStorage.getItem('mind_broker') || defaultBrokerUrl());
  const [generation, setGeneration] = useState(0);
  const [drawerOpen, setDrawerOpen] = useState(false);
  const [now, setNow] = useState(() => Date.now());

  // Tick once a second so ages / stale-offline states stay fresh.
  useEffect(() => {
    const t = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(t);
  }, []);

  const status = useMqtt(url, generation, dispatch);

  const handleConnect = useCallback((next: string) => {
    const u = next || defaultBrokerUrl();
    localStorage.setItem('mind_broker', u);
    setUrl(u);
    setGeneration((g) => g + 1); // force the effect to reconnect
    setDrawerOpen(false);
  }, []);

  return (
    <>
      <Header status={status} url={url} onOpenSettings={() => setDrawerOpen(true)} />
      <ConnectionDrawer
        open={drawerOpen}
        url={url}
        status={status}
        onClose={() => setDrawerOpen(false)}
        onConnect={handleConnect}
      />
      <main>
        <div className="col-main">
          <WearablePanel wearables={Object.values(state.wearables)} now={now} />
          <NodePanel nodes={Object.values(state.nodes)} now={now} />
        </div>
        <IncidentFeed
          incidents={state.incidents}
          onAck={(id) => dispatch({ type: 'ack', id })}
          onAckAll={() => dispatch({ type: 'ackAll' })}
          onClearAcked={() => dispatch({ type: 'clearAcked' })}
        />
      </main>
    </>
  );
}
