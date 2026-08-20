import type { LogicalEvent } from '../state/store';
import { eventLabel, formatDeviceTime } from '../lib/format';

interface IncidentFeedProps {
  events: LogicalEvent[];
  conflictCount: number;
  loading: boolean;
  error?: string;
}

export function IncidentFeed({ events, conflictCount, loading, error }: IncidentFeedProps) {
  return (
    <section className="panel feed-panel" aria-labelledby="event-feed-heading">
      <div className="section-heading">
        <div>
          <p className="eyebrow">Globally deduplicated by wearable and packet</p>
          <h2 id="event-feed-heading">Event feed</h2>
        </div>
        <span className="count">{events.length} logical events</span>
      </div>
      {conflictCount > 0 && <p className="notice warning" role="alert">{conflictCount} conflicting payload report{conflictCount === 1 ? '' : 's'} detected this session.</p>}
      {error && <p className="notice error" role="alert">Event request failed: {error} Retrying while preserving the feed.</p>}
      {events.length === 0 ? (
        <p className="empty">{loading ? 'Loading event records…' : 'No logical events in this bridge session.'}</p>
      ) : (
        <ol className="event-feed">
          {events.map((logical) => {
            const { record } = logical;
            return (
              <li key={logical.key}>
                <article className={`event-record${logical.conflict ? ' conflict' : ''}`}>
                  <div className="record-header">
                    <div>
                      <h3>Wearable {record.wearable} — {eventLabel(record.event)}</h3>
                      <time dateTime={`PT${Math.floor(record.now / 1000)}S`}>Reported at {formatDeviceTime(record.now)}</time>
                    </div>
                    {logical.conflict && <strong className="conflict-flag">Payload conflict</strong>}
                  </div>
                  <dl className="event-data">
                    <div><dt>Confidence</dt><dd>{record.confidence}%</dd></div>
                    <div><dt>SVM</dt><dd>{record.svm}</dd></div>
                    <div><dt>Mic</dt><dd>{record.mic}</dd></div>
                    <div><dt>Sequence</dt><dd>{record.seq}</dd></div>
                  </dl>
                  <h4>Observer evidence ({logical.evidence.length}{logical.evidenceSaturated ? '+' : ''})</h4>
                  <ul className="evidence-list">
                    {logical.evidence.map((evidence) => (
                      <li key={`${evidence.root}:${evidence.observer}:${evidence.path}:${evidence.device}`}>
                        Device {evidence.device} · root <code>{evidence.root}</code> · observer <code>{evidence.observer}</code> · {evidence.path} path
                      </li>
                    ))}
                  </ul>
                  {logical.evidenceSaturated && <p className="overflow">Evidence presentation is saturated; further observer identities are not retained.</p>}
                  <footer><code>Packet ID {record.packet}</code></footer>
                </article>
              </li>
            );
          })}
        </ol>
      )}
    </section>
  );
}
