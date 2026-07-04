import { Wearable } from '../state/store';
import { WearableCard } from './WearableCard';

interface Props {
  wearables: Wearable[];
  now: number;
}

export function WearablePanel({ wearables, now }: Props) {
  const list = [...wearables].sort((a, b) => a.deviceId - b.deviceId);
  return (
    <section>
      <h2>Wearables <span className="count">{list.length}</span></h2>
      {list.length === 0 ? (
        <div className="empty">waiting for wearable beacons…</div>
      ) : (
        <div className="grid">
          {list.map((w) => <WearableCard key={w.deviceId} wearable={w} now={now} />)}
        </div>
      )}
    </section>
  );
}
