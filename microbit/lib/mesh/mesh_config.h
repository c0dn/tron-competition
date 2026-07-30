/*
 * mesh_config.h - compile-time knobs for the flood mesh.
 *
 * Every value here can be overridden by defining it before this header is
 * reached (a -D on the target, or a #define in the app's own config), so an
 * app can retune the mesh without editing shared code. The defaults are the
 * ones the bench work settled on; the reasoning is with each.
 */

#ifndef MESH_CONFIG_H
#define MESH_CONFIG_H

#include "ble_radio.h"      /* BLE_CHAN_MASK_ALL */

/* Master switch. 0 compiles the mesh out of an app entirely: mesh_originate()
   still exists but transmits a plain advert with no mesh header, so a build
   with the mesh off is byte-identical on air to the pre-mesh firmware. */
#ifndef MESH_ENABLE
#  define MESH_ENABLE           1
#endif

/* Role. A pure SOURCE originates but never rebroadcasts anyone else's traffic;
   a pure RELAY does the opposite. BOTH is the wearable default - a wearable
   that refuses to relay is a hole in the mesh exactly where people are. */
#define MESH_ROLE_SOURCE        0
#define MESH_ROLE_RELAY         1
#define MESH_ROLE_BOTH          2

#ifndef MESH_ROLE
#  define MESH_ROLE             MESH_ROLE_BOTH
#endif

/* Initial hop budget for locally originated messages.
 *
 * The field is one byte, so nothing here is forced by the format; 6 is a
 * latency budget. Worst case per hop is one full jitter window plus the
 * transmit itself, so 6 hops is roughly 6 * (50 ms + ~1.2 ms) = ~310 ms. That
 * sits well inside the 1500 ms heartbeat period, so a relayed copy still
 * arrives before the originator's next message and the C3 never sees a record
 * out of order relative to its successor. */
#ifndef MESH_TTL_DEFAULT
#  define MESH_TTL_DEFAULT      6
#endif

/* Relay backoff window, microseconds.
 *
 * Every node that hears a packet wants to rebroadcast it at the same instant.
 * Without a randomised delay they all key the transmitter together and the
 * copies collide, which gets worse as the mesh gets denser - the failure mode
 * is that adding relays reduces delivery. 50 ms against a ~1.2 ms three-channel
 * burst gives roughly a 40:1 slot-to-packet ratio, so two neighbours collide
 * about 2.5% of the time. */
#ifndef MESH_JITTER_MAX_US
#  define MESH_JITTER_MAX_US    50000UL
#endif

/* Rebroadcasts per relayed message, each with an independently drawn backoff.
 *
 * One transmit that collides is lost for good. Two independent draws make a
 * collision on both unlikely, at twice the airtime. Note this multiplies:
 * K repeats over H hops is K^H copies in flight, which is why K stays at 2. */
#ifndef MESH_RELAY_REPEATS
#  define MESH_RELAY_REPEATS    2
#endif

/* Duplicate-suppression cache.
 *
 * A flood without duplicate suppression is a broadcast storm: each node
 * rebroadcasts every copy it hears, including copies of copies. The cache is
 * what makes the flood terminate.
 *
 * 32 entries covers a bench population with room to spare. Aging at 10 s is
 * set against the wearable's ~6 s incident burst (BURST_COUNT * BURST_INTERVAL
 * = 50 * 120 ms): an entry has to outlive the whole burst or the tail of the
 * burst is treated as new and re-floods. */
#ifndef MESH_CACHE_ENTRIES
#  define MESH_CACHE_ENTRIES    32
#endif
#ifndef MESH_CACHE_AGE_MS
#  define MESH_CACHE_AGE_MS     10000UL
#endif

/* What gets relayed.
 *
 * Heartbeats are the bulk of the traffic and the least valuable per packet, so
 * INCIDENTS_ONLY is the lever to pull if airtime becomes the constraint. It is
 * not the default: heartbeats are how the dashboard knows a distant node is
 * alive, and a node that only appears when it falls cannot be distinguished
 * from one that is out of range. */
#define MESH_FLOOD_INCIDENTS    0
#define MESH_FLOOD_ALL          1

#ifndef MESH_FLOOD_CLASS
#  define MESH_FLOOD_CLASS      MESH_FLOOD_ALL
#endif

/* Channels used for rebroadcast.
 *
 * Relaying on one channel would cut relay airtime by two thirds, which is
 * tempting once several nodes rebroadcast. It is NOT safe by default:
 * ble_radio_listen() camps on a single channel, so a relay transmitting only on
 * 37 is inaudible to any node listening on 38. Narrow this only after
 * confirming every node in the deployment camps on the same channel. */
#ifndef MESH_RELAY_CHAN_MASK
#  define MESH_RELAY_CHAN_MASK  BLE_CHAN_MASK_ALL
#endif

/* Channel the relay camps on. The originator transmits on all three each
   event, so listening on one still hears every advertising event. */
#ifndef MESH_LISTEN_CH
#  define MESH_LISTEN_CH        37
#endif

/* Relay backlog depth.
 *
 * The receive loop must never wait, so a message due for rebroadcast is queued
 * and a separate task serves the backoff. Without this the relay is deaf for
 * the whole backoff window, which is not a corner case: a source emitting a
 * few messages back to back gets its first one relayed and the rest missed
 * entirely. Measured before the queue existed, a relay caught 20 of ~80
 * messages from a 4-message-per-second source - the other 60 arrived while it
 * was sitting in its own jitter delay.
 *
 * Eight is sized for a burst arriving faster than the backoff drains it
 * (~100 ms per message at the default window and repeat count). An overflow
 * drops the NEWEST message and counts it, so a message that has already waited
 * is never discarded in favour of one that has not. */
#ifndef MESH_RELAY_QUEUE
#  define MESH_RELAY_QUEUE      8
#endif

/* Priority of the internal backoff/transmit task created by mesh_init().
 *
 * Must be numerically LOWER (i.e. higher priority) than the task running
 * mesh_task(), or the spinning receive loop will never yield to it and nothing
 * will ever be rebroadcast. Keep it below the app's real-time work. */
#ifndef MESH_TX_TASK_PRI
#  define MESH_TX_TASK_PRI      11
#endif

/* Poll cadence.
 *
 * 1 spins the receive loop with no delay; 0 sleeps a tick between polls.
 *
 * Spinning is the default because sleeping loses packets. The receiver lands
 * every packet in one buffer, so anything arriving while the loop is asleep
 * overwrites its predecessor unseen. Measured on the bench: ambient traffic
 * runs ~530 packets/s (one per ~1.9 ms), and a 2 ms sleep - which the tick
 * quantiser can stretch to 3 ms - dropped 55% of them.
 *
 * The cost is that the relay task never blocks, so it must be the LOWEST
 * priority task in the app. Everything above it is timer-driven and preempts
 * on its own schedule; the relay simply consumes what is left. Give this task
 * a priority above any other and it will starve them. */
#ifndef MESH_POLL_SPIN
#  define MESH_POLL_SPIN        1
#endif

#endif /* MESH_CONFIG_H */
