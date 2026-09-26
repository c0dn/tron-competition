# FULL TAVRN 1200-second metrics

These values are aggregated by `microbit/scripts/compare_tavrn_benchmarks.py`
from `final/final.json` and the adjacent `final/control_deltas.csv`.

## Application totals

| Workload | Offered | Accepted | Rejected | Not ready | Delivered | Delivered/offered | Delivered/accepted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Combined | 2,973 | 2,175 | 798 | 56 | 2,076 | 69.8285% | 95.4483% |
| Heartbeat | 1,173 | 1,040 | 133 | 56 | 1,030 | 87.8090% | 99.0385% |
| Throughput | 1,800 | 1,135 | 665 | 0 | 1,046 | 58.1111% | 92.1586% |

## Throughput bursts

| Burst | Window start | Accepted | Delivered | Delivered/accepted | Goodput | p50 latency | p95 latency | p99 latency |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 50 s | 344 | 305 | 88.6628% | 5.0833 pkt/s | 1,635.18 ms | 5,117.60 ms | 5,556.27 ms |
| 1 | 500 s | 407 | 375 | 92.1376% | 6.2500 pkt/s | 1,737.60 ms | 4,755.16 ms | 7,934.25 ms |
| 2 | 950 s | 384 | 366 | 95.3125% | 6.1000 pkt/s | 1,679.30 ms | 4,660.60 ms | 5,121.03 ms |

## Control and transmission proxies

| Proxy | Total |
| --- | ---: |
| Received control packets | 3,246 |
| Received control PDU bytes | 53,321 B |
| AODV-dispatched control packets | 756 |
| AODV-dispatched control PDU bytes | 11,329 B |
| Scheduler TX completed | 12,271 |
| Scheduler TX failed | 0 |
| Link TX completed | 5,778 |
| Link TX failed | 0 |
| Retry due | 980 |
| Retry exhausted | 50 |
| AODV action backpressure | 602 |
| Scheduler faults | 0 |
| Router failure invariants | 0 |

Received-control values aggregate receptions across all six boards. They count
control PDU bytes, not BLE PHY bytes or unique over-the-air transmissions. The
TX control figures cover only the explicitly declared AODV dispatch proxy and
therefore are not a complete control-transmission total.

### Received-control breakdown

| Control type | Packets | PDU bytes |
| --- | ---: | ---: |
| RREQ | 1,019 | 15,476 |
| RREP | 917 | 14,895 |
| RERR | 300 | 4,540 |
| RREP-ACK | 366 | 4,758 |
| HELLO | 357 | 7,024 |
| SYNC-OFFER | 20 | 480 |
| SYNC-PULL | 130 | 2,860 |
| SYNC-DATA | 98 | 2,352 |
| TC-UPDATE | 39 | 936 |
| **Total** | **3,246** | **53,321** |

## GTT

| Metric | Value |
| --- | ---: |
| Formally complete fleet windows | 101 |
| Converged windows | 95 |
| Mean precision | 1.000000 |
| Mean recall | 0.986799 |
| Mean pairwise agreement | 0.983718 |
| Formal validity | `INCOMPLETE` |
| Reason | `gtt_cadence_gap:E` |

Role E resumed reporting after the single 12,034 ms cadence gap. The final nine
fleet windows, from 1110 through 1190 seconds, included all six boards and were
converged.
