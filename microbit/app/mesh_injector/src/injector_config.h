/*
 * injector_config.h - build-time settings for mesh_injector.
 *
 * Flash two boards from the same source with different values here (or -D
 * overrides) to get a source and a relay.
 */

#ifndef INJECTOR_CONFIG_H
#define INJECTOR_CONFIG_H

/* What this node does. SOURCE emits synthetic traffic and never rebroadcasts;
   RELAY only rebroadcasts; BOTH does each. Maps onto mesh_cfg_t.role. */
#define INJ_MODE_SOURCE     0
#define INJ_MODE_RELAY      1
#define INJ_MODE_BOTH       2

#ifndef INJ_MODE
#  define INJ_MODE          INJ_MODE_BOTH
#endif

/* This node's own mesh identity (AdvA[4]). Keep injector ids clear of the
   wearable range so a shared sniffer can tell test traffic from real traffic. */
#ifndef INJ_DEVICE_ID
#  define INJ_DEVICE_ID     0xA0
#endif

/* Synthetic originators.
 *
 * One radio impersonating N originators is how the cache, the TTL logic and
 * the duplicate paths get exercised at a population size we do not have boards
 * for. Every id is a distinct cache key, so N=4 fills four of the 32 slots and
 * makes suppression observable.
 *
 * KNOWN LIMIT: one radio cannot collide with itself. This scales the protocol
 * state, not the RF contention, so it cannot stand in for a real multi-node
 * test of the backoff. Report results from this as protocol-level only. */
#ifndef INJ_VIRTUAL_SRC
#  define INJ_VIRTUAL_SRC   4
#endif
#ifndef INJ_SRC_BASE_ID
#  define INJ_SRC_BASE_ID   0xB0        /* virtual ids 0xB0 .. 0xB0+N-1 */
#endif

/* Gap between injection rounds. One round emits one message per virtual
   source, so the on-air rate is INJ_VIRTUAL_SRC messages per interval. */
#ifndef INJ_INTERVAL_MS
#  define INJ_INTERVAL_MS   1000
#endif

/* Emit the edge-case probes described in main.c every Nth round. These are the
   packets a healthy mesh can never produce on its own - an exhausted hop
   budget, a message already several hops old, an exact duplicate - so without
   them those branches are never taken on the bench. Set 0 for clean traffic. */
#ifndef INJ_TEST_EVERY
#  define INJ_TEST_EVERY    5
#endif
#ifndef INJ_TEST_ID
#  define INJ_TEST_ID       0xBF        /* probe traffic, distinct from sources */
#endif

/* Serial stats cadence. */
#ifndef INJ_STATS_MS
#  define INJ_STATS_MS      5000
#endif

/* Transmit power, dBm. Turning a source down is what forces a relay topology
   onto a desk: at -40 the far receiver cannot hear the source directly, so
   anything it does hear arrived through the relay. Set 127 to leave the
   radio's default alone. */
#ifndef INJ_TXPOWER_DBM
#  define INJ_TXPOWER_DBM   127
#endif

#endif /* INJECTOR_CONFIG_H */
