#ifndef TAVRN_WIRE_V2_H
#define TAVRN_WIRE_V2_H

#include <stddef.h>
#include <stdint.h>

#define TAVRN_ADVA_LEN             6u
#define TAVRN_LINK_APP_BYTES       10u
#define TAVRN_LINK_CONTROL_PDU_MAX 24u

typedef char tavrn_wire_v2_pdu_budget_guard[
    (TAVRN_LINK_CONTROL_PDU_MAX == 24u) ? 1 : -1];
typedef char tavrn_wire_v2_app_budget_guard[
    (TAVRN_LINK_APP_BYTES == 10u) ? 1 : -1];

typedef struct tavrn_adva {
    uint8_t bytes[TAVRN_ADVA_LEN];
} tavrn_adva_t;

typedef enum tavrn_identity_width {
    TAVRN_IDENTITY_SID8 = 1,
    TAVRN_IDENTITY_SID16 = 2,
} tavrn_identity_width_t;

typedef struct tavrn_logical_id {
    tavrn_identity_width_t width;
    uint16_t value;
} tavrn_logical_id_t;

typedef struct tavrn_direct_peer {
    tavrn_logical_id_t logical_id;
    tavrn_adva_t adva;
} tavrn_direct_peer_t;

typedef struct tron_application_data {
    tavrn_logical_id_t final_destination;
    uint8_t app_kind;
    uint8_t app_source;
    uint8_t urgent;
    uint8_t app_len;
    uint8_t app_bytes[TAVRN_LINK_APP_BYTES];
} tron_application_data_t;

typedef enum tavrn_data_ownership {
    TAVRN_DATA_ORIGINATED = 0,
    TAVRN_DATA_TRANSIT = 1,
} tavrn_data_ownership_t;

typedef struct tavrn_link_data {
    tavrn_logical_id_t origin;
    tavrn_logical_id_t final_destination;
    uint16_t data_seq;
    uint8_t ttl;
    uint8_t hops;
    uint8_t app_kind;
    uint8_t app_source;
    uint8_t urgent;
    uint8_t app_len;
    uint8_t app_bytes[TAVRN_LINK_APP_BYTES];
    tavrn_data_ownership_t ownership;
} tavrn_link_data_t;

typedef enum tavrn_hack_status {
    TAVRN_HACK_ACCEPTED = 0,
    TAVRN_HACK_DUPLICATE = 1,
    TAVRN_HACK_BUSY = 2,
    TAVRN_HACK_REJECTED = 3,
} tavrn_hack_status_t;

typedef enum tavrn_wire_type {
    TAVRN_WIRE_E_RREQ = 0x01,
    TAVRN_WIRE_E_RREP = 0x02,
    TAVRN_WIRE_E_RERR = 0x03,
    TAVRN_WIRE_HELLO = 0x04,
    TAVRN_WIRE_SYNC_OFFER = 0x05,
    TAVRN_WIRE_SYNC_PULL = 0x06,
    TAVRN_WIRE_SYNC_DATA = 0x07,
    TAVRN_WIRE_TC_UPDATE = 0x08,
    TAVRN_WIRE_E_RREP_ACK = 0x09,
    TAVRN_WIRE_DATA = 0x10,
    TAVRN_WIRE_HACK = 0x11,
    TAVRN_WIRE_FLOOD = 0x12,
} tavrn_wire_type_t;

typedef enum tavrn_codec_result {
    TAVRN_CODEC_OK = 0,
    TAVRN_CODEC_INVALID_ARGUMENT,
    TAVRN_CODEC_MALFORMED_WRAPPER,
    TAVRN_CODEC_MALFORMED_EXACT_LENGTH,
    TAVRN_CODEC_MALFORMED_FLAGS,
    TAVRN_CODEC_MALFORMED_FIELD,
    TAVRN_CODEC_FOREIGN_NETWORK,
    TAVRN_CODEC_UNSUPPORTED_TYPE,
    TAVRN_CODEC_IDENTITY_CONFLICT,
    TAVRN_CODEC_OUTPUT_TOO_SMALL,
} tavrn_codec_result_t;

typedef int (*tavrn_codec_identity_conflict_fn)(
    void *context, const tavrn_logical_id_t *logical_id,
    const tavrn_adva_t *direct_adva_or_null);

typedef struct tavrn_codec_config {
    uint8_t network_id;
    tavrn_direct_peer_t local_peer;
    tavrn_codec_identity_conflict_fn identity_conflict;
    void *identity_context;
} tavrn_codec_config_t;

typedef struct tavrn_codec_data {
    tavrn_logical_id_t immediate_receiver;
    tavrn_link_data_t data;
} tavrn_codec_data_t;

typedef struct tavrn_codec_hack {
    tavrn_logical_id_t immediate_receiver;
    tavrn_logical_id_t data_origin;
    tavrn_logical_id_t final_destination;
    uint16_t data_seq;
    uint8_t app_kind;
    uint8_t app_source;
    tavrn_hack_status_t status;
} tavrn_codec_hack_t;

#define TAVRN_FLOOD_BODY_MAX 12u

typedef struct tavrn_codec_flood {
    tavrn_logical_id_t origin;
    uint16_t flood_seq;
    uint8_t ttl;
    uint8_t hops;
    uint8_t urgent;
    uint8_t flood_class;
    uint8_t body_len;
    uint8_t body[TAVRN_FLOOD_BODY_MAX];
} tavrn_codec_flood_t;

typedef struct tavrn_validated_control {
    tavrn_wire_type_t type;
    uint8_t pdu_len;
    uint8_t pdu[TAVRN_LINK_CONTROL_PDU_MAX];
} tavrn_validated_control_t;

typedef struct tavrn_decoded_frame {
    tavrn_wire_type_t type;
    uint8_t network_id;
    tavrn_direct_peer_t transmitter;
    union {
        tavrn_codec_data_t data;
        tavrn_codec_hack_t hack;
        tavrn_codec_flood_t flood;
        tavrn_validated_control_t control;
    } detail;
} tavrn_decoded_frame_t;

tavrn_codec_result_t tavrn_wire_v2_decode(
    const tavrn_codec_config_t *config, const uint8_t outer_adva[TAVRN_ADVA_LEN],
    const uint8_t *adv_data, size_t adv_len,
    tavrn_decoded_frame_t *frame_out);

tavrn_codec_result_t tavrn_wire_v2_encode(
    const tavrn_codec_config_t *config,
    const tavrn_decoded_frame_t *frame,
    uint8_t *adv_data_out, size_t adv_capacity, size_t *adv_len_out);

#endif /* TAVRN_WIRE_V2_H */
