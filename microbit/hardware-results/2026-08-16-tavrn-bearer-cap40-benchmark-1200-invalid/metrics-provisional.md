# Provisional PDR, goodput, and latency

The repository's `summarize_tavrn_benchmark.py` generated the underlying
`app_windows.csv` and `throughput_bursts.csv`. Values marked invalid are shown
for diagnosis only and must not be used as accepted benchmark results.

## Valid heartbeat prefix

| Window (s) | Offered | Accepted | Delivered | Offered PDR | Packet goodput | App goodput | p50 (ms) | p95 (ms) | p99 (ms) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0–10 (startup) | 10 | 6 | 6 | 60% | 0.6 pkt/s | 4.2 B/s | 7.542 | 64.368 | 64.368 |
| 10–20 | 10 | 10 | 10 | 100% | 1.0 pkt/s | 7.0 B/s | 6.854 | 8.819 | 8.819 |
| 20–30 | 10 | 10 | 10 | 100% | 1.0 pkt/s | 7.0 B/s | 7.097 | 9.027 | 9.027 |
| 30–40 | 10 | 10 | 10 | 100% | 1.0 pkt/s | 7.0 B/s | 7.270 | 8.444 | 8.444 |
| 40–50 | 10 | 10 | 10 | 100% | 1.0 pkt/s | 7.0 B/s | 6.929 | 9.756 | 9.756 |

The four post-startup valid windows delivered 40/40 packets. Their per-window
p50 range was 6.854–7.270 ms and p95 range was 8.444–9.756 ms.

## First throughput burst — invalid telemetry

| Window | Offered | Accepted | Delivered | Offered PDR | Accepted PDR | Packet goodput | App goodput | p50 (ms) | p95 (ms) | p99 (ms) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Burst 0, 60 s | 395 | 165 | 36 | 9.114% | 21.818% | 0.6 pkt/s | 4.2 B/s | 5931.985 | 8663.307 | 9240.344 |

This burst is incomplete and invalid (`A`, `B`, and `D` affected). The values
are lower-bound/provisional observations, not a valid measurement of the full
600-packet offered burst.

## All recorded source packets — invalid aggregate

| Workload | Offered records | Attempted | Accepted | Delivered | Offered PDR | Accepted PDR | p50 (ms) | p95 (ms) | p99 (ms) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Heartbeat | 100 | 95 | 67 | 57 | 57.000% | 85.075% | 7.438 | 64.368 | 284.307 |
| Throughput | 395 | 395 | 165 | 36 | 9.114% | 21.818% | 5931.985 | 8663.307 | 9240.344 |
| Combined | 495 | 490 | 232 | 93 | 18.788% | 40.086% | 8.403 | 7546.396 | 9240.344 |

The aggregate excludes 866 unmatched final records and all offers lost after
telemetry overflow. It describes only records retained by the invalid capture.
