import { useState } from 'react';
import { Incident } from '../state/store';

type Filter = 'all' | 'critical' | 'unacked';

interface Props {
  incidents: Incident[];
  onAck: (id: number) => void;
  onAckAll: () => void;
  onClearAcked: () => void;
}

export function IncidentFeed({ incidents, onAck, onAckAll, onClearAcked }: Props) {
  const [filter, setFilter] = useState<Filter>('all');

  const shown = incidents.filter((i) => {
    if (filter === 'critical') return i.severity === 3;
    if (filter === 'unacked') return !i.acked;
    return true;
  });
  const unackedCount = incidents.filter((i) => !i.acked).length;

  return (
    <section className="feed-section">
      <h2>
        Incident Feed
        {unackedCount > 0 && <span className="count alert">{unackedCount} new</span>}
      </h2>

      <div className="feed-controls">
        <div className="filters">
          {(['all', 'critical', 'unacked'] as Filter[]).map((f) => (
            <button
              key={f}
              className={`chip${filter === f ? ' active' : ''}`}
              onClick={() => setFilter(f)}
            >
              {f}
            </button>
          ))}
        </div>
        <div className="feed-actions">
          <button className="chip" onClick={onAckAll} disabled={unackedCount === 0}>
            ack all
          </button>
          <button className="chip" onClick={onClearAcked}>clear acked</button>
        </div>
      </div>

      {shown.length === 0 ? (
        <div className="empty">no incidents</div>
      ) : (
        <div className="feed">
          {shown.map((i) => (
            <div key={i.id} className={`feed-row sev${i.severity}${i.acked ? ' acked' : ''}`}>
              <div className="feed-main">
                <span className="feed-time">{new Date(i.ts).toLocaleTimeString()}</span>
                <span className="feed-name">{i.name}</span>
                <span className="feed-meta">
                  wearable #{i.deviceId} · conf {i.confidence} · via #{i.nodeId} · {i.rssi} dBm
                </span>
              </div>
              {!i.acked && (
                <button className="ack-btn" onClick={() => onAck(i.id)} title="acknowledge">
                  ✓
                </button>
              )}
            </div>
          ))}
        </div>
      )}
    </section>
  );
}
