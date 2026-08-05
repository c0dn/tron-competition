#ifndef TRON_MESH_PINGPONG_H
#define TRON_MESH_PINGPONG_H

/*
 * Pure application protocol for the fixed two-board addressed PING/PONG test.
 * Delivery is best-effort over the existing advertisement flood: there are no
 * acknowledgements, retries, or overlapping root transactions.
 */

#include <stdint.h>

#include "tron_mesh_packet.h"
#include "tron_timer_config.h"

#define TRON_MESH_PINGPONG_NET_ID          0x01u
#define TRON_MESH_PINGPONG_ROOT_ID         0x0001u
#define TRON_MESH_PINGPONG_LEAF_ID         0x0002u
#define TRON_MESH_PINGPONG_PAYLOAD_LEN     2u
#define TRON_MESH_PINGPONG_ADV_LEN         (TRON_MESH_ADV_BASE_LEN + TRON_MESH_PINGPONG_PAYLOAD_LEN)
typedef enum tron_mesh_pingpong_result {
    TRON_MESH_PINGPONG_OK = 0,
    TRON_MESH_PINGPONG_ERR_NULL,
    TRON_MESH_PINGPONG_ERR_TYPE,
    TRON_MESH_PINGPONG_ERR_NETWORK,
    TRON_MESH_PINGPONG_ERR_TTL,
    TRON_MESH_PINGPONG_ERR_PAYLOAD_LEN,
    TRON_MESH_PINGPONG_ERR_SOURCE,
    TRON_MESH_PINGPONG_ERR_DESTINATION,
} tron_mesh_pingpong_result_t;

typedef struct tron_mesh_pingpong_root {
    const tron_timer_config_t *timers;
    uint32_t next_ping_at_ms;
    uint32_t pending_seq24;
    uint32_t pending_queued_at_ms;
    uint32_t pending_deadline_ms;
    uint8_t pending;
} tron_mesh_pingpong_root_t;

typedef enum tron_mesh_pingpong_attempt_result {
    TRON_MESH_PINGPONG_ATTEMPT_NOT_DUE = 0,
    TRON_MESH_PINGPONG_ATTEMPT_QUEUE_FAILED,
    TRON_MESH_PINGPONG_ATTEMPT_QUEUED,
} tron_mesh_pingpong_attempt_result_t;

typedef enum tron_mesh_pingpong_match_result {
    TRON_MESH_PINGPONG_PONG_UNMATCHED = 0,
    TRON_MESH_PINGPONG_PONG_MATCHED,
    TRON_MESH_PINGPONG_PONG_LATE,
} tron_mesh_pingpong_match_result_t;

int tron_mesh_pingpong_is_supported_type(uint8_t msg_type);

/* Validates the fixed root->leaf PING or leaf->root PONG endpoints, including
   exact two-byte little-endian destination payload. TTL zero is valid. */
tron_mesh_pingpong_result_t tron_mesh_pingpong_validate(
    const tron_mesh_packet_t *packet);

tron_mesh_pingpong_result_t tron_mesh_pingpong_build_ping(
    tron_mesh_packet_t *packet,
    uint8_t ttl,
    uint32_t seq24);

/* The input PING must pass endpoint validation. The resulting PONG echoes its
   low 24-bit sequence for correlation. */
tron_mesh_pingpong_result_t tron_mesh_pingpong_build_pong(
    tron_mesh_packet_t *packet,
    const tron_mesh_packet_t *ping,
    uint8_t ttl);

void tron_mesh_pingpong_root_init(tron_mesh_pingpong_root_t *state,
                                  uint32_t now_ms);

int tron_mesh_pingpong_root_ping_due(const tron_mesh_pingpong_root_t *state,
                                     uint32_t now_ms);

/* Call once after each due queue attempt. The periodic deadline advances on a
   failed queue, but pending state starts only when queue_succeeded is nonzero. */
tron_mesh_pingpong_attempt_result_t tron_mesh_pingpong_root_record_ping_attempt(
    tron_mesh_pingpong_root_t *state,
    uint32_t seq24,
    uint32_t now_ms,
    int queue_succeeded);

/* Returns nonzero and clears pending state once its deadline is reached. */
int tron_mesh_pingpong_root_check_timeout(tron_mesh_pingpong_root_t *state,
                                          uint32_t now_ms);

/* A match requires a valid leaf->root PONG, the pending seq24, and arrival
   strictly before the timeout deadline. RTT is enqueue-to-match latency. */
tron_mesh_pingpong_match_result_t tron_mesh_pingpong_root_match_pong(
    tron_mesh_pingpong_root_t *state,
    const tron_mesh_packet_t *pong,
    uint32_t now_ms,
    uint32_t *rtt_ms);

const char *tron_mesh_pingpong_result_name(tron_mesh_pingpong_result_t result);

#endif /* TRON_MESH_PINGPONG_H */
