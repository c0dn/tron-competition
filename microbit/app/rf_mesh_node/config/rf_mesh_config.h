#ifndef RF_MESH_NODE_CONFIG_H
#define RF_MESH_NODE_CONFIG_H

#include "rf_mesh_packet.h"

#define RF_MESH_NODE_RF_CHANNEL_MAX 83u

/*
 * rf_mesh_node v2 compile-time configuration.
 *
 * CMake exposes these as cache variables for build-time role selection, and
 * this header also supplies defaults when the target is compiled directly.
 * Payloads remain dummy/test-only; these constants do not define a real
 * application payload schema.
 */

#ifndef RF_MESH_NODE_ID
#define RF_MESH_NODE_ID 0u          /* 0 => derive a non-root id from FICR */
#endif

#ifndef RF_MESH_IS_ROOT
#define RF_MESH_IS_ROOT 0u          /* 1 => configured root role */
#endif

#ifndef RF_MESH_NETWORK_ID
#define RF_MESH_NETWORK_ID RF_MESH_DEFAULT_NETWORK_ID
#endif

#ifndef RF_MESH_ROOT_ID
#define RF_MESH_ROOT_ID RF_MESH_DEFAULT_ROOT_ID
#endif

#ifndef RF_MESH_RF_CHANNEL
#define RF_MESH_RF_CHANNEL 7u       /* 0..83 => 2400 MHz + channel */
#endif

#ifndef RF_MESH_RF_TXPOWER
#define RF_MESH_RF_TXPOWER 0x00     /* raw nRF RADIO TXPOWER field, 0 dBm */
#endif

#if RF_MESH_IS_ROOT != 0u && RF_MESH_IS_ROOT != 1u
#error "RF_MESH_IS_ROOT must be 0 or 1"
#endif

#if RF_MESH_NODE_ID > 0xffffu
#error "RF_MESH_NODE_ID must fit in 16 bits"
#endif

#if RF_MESH_ROOT_ID > 0xffffu
#error "RF_MESH_ROOT_ID must fit in 16 bits"
#endif

#if RF_MESH_NETWORK_ID > 0xffffu
#error "RF_MESH_NETWORK_ID must fit in 16 bits"
#endif

#if RF_MESH_ROOT_ID == 0u || RF_MESH_ROOT_ID == RF_MESH_BROADCAST_ID
#error "RF_MESH_ROOT_ID must not be 0 or broadcast"
#endif

#if RF_MESH_NODE_ID == RF_MESH_BROADCAST_ID
#error "RF_MESH_NODE_ID must not be broadcast"
#endif

#if RF_MESH_IS_ROOT == 0u && RF_MESH_NODE_ID != 0u && RF_MESH_NODE_ID == RF_MESH_ROOT_ID
#error "Non-root RF_MESH_NODE_ID must not equal RF_MESH_ROOT_ID"
#endif

#if RF_MESH_RF_CHANNEL > RF_MESH_NODE_RF_CHANNEL_MAX
#error "RF_MESH_RF_CHANNEL must be in 0..83"
#endif

#if RF_MESH_RF_TXPOWER != 0x08 && RF_MESH_RF_TXPOWER != 0x04 && \
    RF_MESH_RF_TXPOWER != 0x00 && RF_MESH_RF_TXPOWER != 0xFC && \
    RF_MESH_RF_TXPOWER != 0xF8 && RF_MESH_RF_TXPOWER != 0xF4 && \
    RF_MESH_RF_TXPOWER != 0xF0 && RF_MESH_RF_TXPOWER != 0xEC && \
    RF_MESH_RF_TXPOWER != 0xD8
#error "RF_MESH_RF_TXPOWER must be a supported nRF52833 TXPOWER field value"
#endif

#ifndef RF_MESH_APP_ROOT_BEACON_INTERVAL_MS
#define RF_MESH_APP_ROOT_BEACON_INTERVAL_MS RF_MESH_ROOT_BEACON_INTERVAL_MS
#endif

#ifndef RF_MESH_APP_DUMMY_DATA_INTERVAL_MS
#define RF_MESH_APP_DUMMY_DATA_INTERVAL_MS RF_MESH_DUMMY_DATA_INTERVAL_MS
#endif

#ifndef RF_MESH_APP_OBSERVABILITY_INTERVAL_MS
#define RF_MESH_APP_OBSERVABILITY_INTERVAL_MS 1000u
#endif

#ifndef RF_MESH_APP_IDLE_DELAY_MS
#define RF_MESH_APP_IDLE_DELAY_MS 1u
#endif

#endif /* RF_MESH_NODE_CONFIG_H */
