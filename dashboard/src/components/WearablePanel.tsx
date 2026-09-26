import type { LatestWearable } from '../state/store';
import { WearableCard } from './WearableCard';

interface WearablePanelProps {
  wearables: Record<number, LatestWearable>;
  now: number;
}

export function WearablePanel({ wearables, now }: WearablePanelProps) {
  const list = Object.values(wearables).sort((left, right) => left.record.wearable - right.record.wearable);
  return (
    <section className="panel" aria-labelledby="wearable-state-heading">
      <div className="section-heading">
        <div>
          <p className="eyebrow">Latest report by wearer</p>
          <h2 id="wearable-state-heading">Wearable state</h2>
        </div>
        <span className="count">{list.length} tracked</span>
      </div>
      {list.length === 0 ? (
        <p className="empty">No wearable reports received yet. The bridge will add the latest state when an event arrives.</p>
      ) : (
        <ul className="wearable-grid">
          {list.map((wearable) => (
            <li key={wearable.record.wearable}>
              <WearableCard wearable={wearable} now={now} />
            </li>
          ))}
        </ul>
      )}
    </section>
  );
}
