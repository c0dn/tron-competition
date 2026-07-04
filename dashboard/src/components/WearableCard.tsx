import { eventTypeInfo } from '../lib/uplink';
import { fmtAge } from '../lib/format';
import { Wearable, isWearableStale } from '../state/store';
import { Sparkline } from './Sparkline';

interface Props {
  wearable: Wearable;
  now: number;
}

export function WearableCard({ wearable: w, now }: Props) {
  const info = eventTypeInfo(w.eventType);
  const stale = isWearableStale(w, now);

  return (
    <div className={`card sev${info.severity}${stale ? ' stale' : ''}${info.severity === 3 ? ' pulse' : ''}`}>
      <div className="card-top">
        <span className="id">wearable #{w.deviceId}</span>
        <span className={`badge sev${info.severity}`}>{info.name}</span>
      </div>

      <div className="metric-row">
        <div className="metric"><span>conf</span><b>{w.confidence}</b></div>
        <div className="metric"><span>SVM</span><b>{w.accelSvm}<small>mg</small></b></div>
        <div className="metric"><span>mic</span><b>{w.micLevel}</b></div>
      </div>

      <div className="spark-row">
        <span className="spark-label">accel SVM</span>
        <Sparkline values={w.svmHistory} />
      </div>

      <div className="card-foot">
        <span>via node #{w.nodeId} · {w.rssi} dBm</span>
        <span>{fmtAge(now - w.lastSeen)}</span>
      </div>
    </div>
  );
}
