import { MindNode } from '../state/store';
import { NodeCard } from './NodeCard';

interface Props {
  nodes: MindNode[];
  now: number;
}

export function NodePanel({ nodes, now }: Props) {
  const list = [...nodes].sort((a, b) => a.nodeId - b.nodeId);
  return (
    <section style={{ marginTop: 22 }}>
      <h2>ESP32-C3 Nodes <span className="count">{list.length}</span></h2>
      {list.length === 0 ? (
        <div className="empty">waiting for nodes…</div>
      ) : (
        <div className="grid">
          {list.map((n) => <NodeCard key={n.nodeId} node={n} now={now} />)}
        </div>
      )}
    </section>
  );
}
