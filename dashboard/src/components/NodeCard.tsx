import { fmtAge, fmtUptime, fmtHeap } from '../lib/format';
import { MindNode, isNodeOnline } from '../state/store';

interface Props {
  node: MindNode;
  now: number;
}

export function NodeCard({ node: n, now }: Props) {
  const online = isNodeOnline(n, now);
  return (
    <div className={`card node${online ? '' : ' stale'}`}>
      <div className="card-top">
        <span className="id">
          node #{n.nodeId}
          {n.isRoot && <span className="root">ROOT</span>}
        </span>
        <span className={`dot ${online ? 'up' : 'down'}`} />
      </div>

      <div className="metric-row">
        <div className="metric"><span>level</span><b>{n.meshLevel ?? '–'}</b></div>
        <div className="metric"><span>children</span><b>{n.childCount ?? '–'}</b></div>
        <div className="metric"><span>parent</span><b>{n.parentRssi ?? '–'}<small>dBm</small></b></div>
      </div>

      <div className="card-foot">
        <span>
          heap {n.freeHeap != null ? fmtHeap(n.freeHeap) : '–'} · up{' '}
          {n.uptimeS != null ? fmtUptime(n.uptimeS) : '–'}
        </span>
        <span>{n.lastSeen != null ? fmtAge(now - n.lastSeen) : '—'}</span>
      </div>
    </div>
  );
}
