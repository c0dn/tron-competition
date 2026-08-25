import { eventLabel, formatDeviceTime } from '../lib/format';
import type { IncidentLocalizationView } from '../localization/IncidentLocalization';

interface IncidentFeedProps {
  views: readonly IncidentLocalizationView[];
  conflictCount: number;
  loading: boolean;
  error?: string;
}

function eventErrorMessage(error: string): string {
  return /(?:\/api\/root|root(?:\b|_))/i.test(error)
    ? 'The event feed is temporarily unavailable.'
    : error;
}

function locationAnnouncement(views: readonly IncidentLocalizationView[]): string {
  if (views.length === 0) return 'No incident location states.';
  return views.slice(0, 10)
    .map((view) => {
      const identity = `Wearable ${view.logicalEvent.record.wearable}, packet ${view.logicalEvent.record.packet}`;
      if (view.status === 'collecting') return `${identity}: collecting.`;
      if (view.status === 'insufficient') return `${identity}: insufficient, ${view.contributorCount} of 3 contributors.`;
      const spread = view.normalizedSpread === null ? 'not available' : view.normalizedSpread.toFixed(2);
      return `${identity}: ballpark at normalized coordinates ${view.x.toFixed(2)}, ${view.y.toFixed(2)}, ${view.contributorCount} contributors, normalized spread ${spread}.`;
    })
    .join(' ');
}

export function IncidentFeed({ views, conflictCount, loading, error }: IncidentFeedProps) {
  return (
    <section className="panel feed-panel" aria-labelledby="event-feed-heading">
      <div className="section-heading">
        <div>
          <p className="eyebrow">Recent incidents</p>
          <h2 id="event-feed-heading">Event feed</h2>
        </div>
        <span className="count">{views.length} logical event{views.length === 1 ? '' : 's'}</span>
      </div>
      <p className="sr-only" role="status" aria-atomic="true">{locationAnnouncement(views)}</p>
      {conflictCount > 0 && <p className="notice warning" role="alert">{conflictCount} conflicting payload report{conflictCount === 1 ? '' : 's'} detected this session.</p>}
      {error && <p className="notice error" role="alert">Event request failed: {eventErrorMessage(error)} Retrying while preserving the feed.</p>}
      {views.length === 0 ? (
        <p className="empty">{loading ? 'Loading event records…' : 'No logical events in this bridge session.'}</p>
      ) : (
        <ol className="event-feed">
          {views.map((view) => {
            const logical = view.logicalEvent;
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
                      <li key={`${evidence.root}:${evidence.observer}:${evidence.path}:${evidence.device}:${evidence.observerRssiDbm ?? 'unavailable'}`}>
                        Observer <code>{evidence.observer}</code> · strongest RSSI {evidence.observerRssiDbm === null ? 'unavailable' : `${evidence.observerRssiDbm} dBm`} · {evidence.sampleCount} sample{evidence.sampleCount === 1 ? '' : 's'} · {evidence.path} path
                      </li>
                    ))}
                  </ul>
                  {logical.evidenceSaturated && <p className="overflow">Evidence presentation is saturated; further observer identities are not retained.</p>}
                  <h4>Location</h4>
                  {view.status === 'collecting' ? <p className="location-status">Collecting</p>
                    : view.status === 'insufficient' ? <p className="location-status">Insufficient — {view.contributorCount} of 3 positioned RSSI contributors</p>
                       : (
                         <dl className="localization-data">
                           <div><dt>Location</dt><dd>Ballpark</dd></div>
                          <div><dt>Contributors</dt><dd>{view.contributorCount}</dd></div>
                          <div><dt>Geometry</dt><dd>{view.geometryWarning ? 'Warning' : 'No warning'}</dd></div>
                          <div><dt>Normalized spread</dt><dd>{view.normalizedSpread === null ? 'Not available' : view.normalizedSpread.toFixed(2)}</dd></div>
                          <div><dt>Normalized coordinates</dt><dd>{view.x.toFixed(2)}, {view.y.toFixed(2)}</dd></div>
                        </dl>
                      )}
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
