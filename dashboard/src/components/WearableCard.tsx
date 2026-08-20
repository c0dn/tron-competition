import type { LatestWearable } from '../state/store';
import { eventLabel, formatAge, formatDeviceTime } from '../lib/format';

interface WearableCardProps {
  wearable: LatestWearable;
  now: number;
}

export function WearableCard({ wearable, now }: WearableCardProps) {
  const { record } = wearable;
  const stale = now - wearable.receivedAt >= 15_000;
  return (
    <article className={`wearable-card${stale ? ' stale' : ''}`}>
      <div className="record-header">
        <div>
          <h3>Wearable {record.wearable}</h3>
          <p>{eventLabel(record.event)}{stale ? ' — stale' : ''}</p>
        </div>
        <time dateTime={`PT${Math.floor(record.now / 1000)}S`}>{formatDeviceTime(record.now)}</time>
      </div>
      <dl className="measurements">
        <div><dt>Confidence</dt><dd>{record.confidence}%</dd></div>
        <div><dt>SVM</dt><dd>{record.svm}</dd></div>
        <div><dt>Mic</dt><dd>{record.mic}</dd></div>
        <div><dt>Sequence</dt><dd>{record.seq}</dd></div>
      </dl>
      <footer>
        <span>Last received {formatAge(Math.max(0, now - wearable.receivedAt))}</span>
        <code>Packet {record.packet}</code>
      </footer>
    </article>
  );
}
