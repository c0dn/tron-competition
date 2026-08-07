#ifndef TRON_MESH_PACKET_H
#define TRON_MESH_PACKET_H

/*
 * Experimental TRON BLE advertising mesh-like packet format.
 *
 * This is intentionally not Bluetooth Mesh compliant. It uses a lab-only
 * Manufacturer Specific Data AD structure rather than the Bluetooth Mesh
 * Mesh Message AD type because this firmware does not implement provisioning,
 * mesh security, replay protection, IV index, SAR, or mesh models.
 */

#include <stdint.h>

#ifndef __TK_TKERNEL_H__
#include <stddef.h>
#endif

#define TRON_MESH_ADV_MAX_LEN             31u
#define TRON_MESH_FLAGS_LEN               3u
#define TRON_MESH_FLAGS_AD_LEN            0x02u
#define TRON_MESH_FLAGS_AD_TYPE           0x01u
#define TRON_MESH_FLAGS_VALUE             0x06u

#define TRON_MESH_MANUF_AD_TYPE           0xFFu
#define TRON_MESH_COMPANY_ID              0xFFFFu
#define TRON_MESH_MAGIC0                  'T'
#define TRON_MESH_MAGIC1                  'M'
#define TRON_MESH_VERSION                 0x01u

#define TRON_MESH_MSG_TYPE_DUMMY_STATUS   0x01u
/* Wearable incident payload: the 7-byte mind_adv_payload_t from
 * shared/schema.h carried verbatim. It fits inside TRON_MESH_PAYLOAD_MAX with
 * room to spare, so the wearable rides the mesh without its own wire format
 * and inherits relay, TTL and duplicate suppression. */
#define TRON_MESH_MSG_TYPE_MIND_EVENT     0x02u
#define TRON_MESH_TTL_MAX                 3u
#define TRON_MESH_PAYLOAD_MAX             12u
#define TRON_MESH_SEQ24_MAX               0xFFFFFFu

/* Manufacturer AD length byte = AD type + company + inner TRON PDU. */
#define TRON_MESH_MANUF_BASE_LEN          0x0Fu
#define TRON_MESH_MANUF_MAX_LEN           (TRON_MESH_MANUF_BASE_LEN + TRON_MESH_PAYLOAD_MAX)

/* Total AdvData length = Flags AD structure + Manufacturer AD structure. */
#define TRON_MESH_ADV_BASE_LEN            (TRON_MESH_FLAGS_LEN + 1u + TRON_MESH_MANUF_BASE_LEN)

typedef struct tron_mesh_packet {
    uint8_t net_id;
    uint8_t ttl;
    uint16_t src;
    /* Only the low 24 bits are encoded on air; higher bits are masked. */
    uint32_t seq24;
    uint8_t msg_type;
    uint8_t payload_len;
    uint8_t payload[TRON_MESH_PAYLOAD_MAX];
} tron_mesh_packet_t;

typedef enum tron_mesh_packet_result {
    TRON_MESH_PACKET_OK = 0,
    TRON_MESH_PACKET_ERR_NULL,
    TRON_MESH_PACKET_ERR_LENGTH,
    TRON_MESH_PACKET_ERR_MISSING_FLAGS,
    TRON_MESH_PACKET_ERR_MISSING_MANUFACTURER,
    TRON_MESH_PACKET_ERR_COMPANY,
    TRON_MESH_PACKET_ERR_MAGIC,
    TRON_MESH_PACKET_ERR_VERSION,
    TRON_MESH_PACKET_ERR_PAYLOAD_LEN,
    TRON_MESH_PACKET_ERR_TTL,
} tron_mesh_packet_result_t;

size_t tron_mesh_packet_encoded_len(uint8_t payload_len);

tron_mesh_packet_result_t tron_mesh_packet_encode(
    const tron_mesh_packet_t *packet,
    uint8_t *adv,
    size_t adv_cap,
    size_t *adv_len);

tron_mesh_packet_result_t tron_mesh_packet_decode(
    const uint8_t *adv,
    size_t adv_len,
    tron_mesh_packet_t *packet);

/* Admission policy helper used before node dedupe, delivery, or relay. */
int tron_mesh_packet_is_for_network(const tron_mesh_packet_t *packet,
                                    uint8_t expected_net_id);

const char *tron_mesh_packet_result_name(tron_mesh_packet_result_t result);

#endif /* TRON_MESH_PACKET_H */
