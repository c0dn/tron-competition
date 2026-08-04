/* Test-only RED codec port. Never link this file into firmware. */
#include "tavrn_wire_v2.h"

#include <string.h>

tavrn_codec_result_t tavrn_wire_v2_decode(
    const tavrn_codec_config_t *config, const uint8_t outer_adva[TAVRN_ADVA_LEN],
    const uint8_t *adv_data, size_t adv_len, tavrn_decoded_frame_t *frame_out)
{
    (void)config;
    (void)outer_adva;
    (void)adv_data;
    (void)adv_len;
    if (frame_out != NULL) {
        memset(frame_out, 0, sizeof(*frame_out));
    }
    return TAVRN_CODEC_UNSUPPORTED_TYPE;
}

tavrn_codec_result_t tavrn_wire_v2_encode(
    const tavrn_codec_config_t *config, const tavrn_decoded_frame_t *frame,
    uint8_t *adv_data_out, size_t adv_capacity, size_t *adv_len_out)
{
    (void)config;
    (void)frame;
    (void)adv_data_out;
    (void)adv_capacity;
    (void)adv_len_out;
    return TAVRN_CODEC_UNSUPPORTED_TYPE;
}
