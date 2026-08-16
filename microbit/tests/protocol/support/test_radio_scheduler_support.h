#ifndef TEST_RADIO_SCHEDULER_SUPPORT_H
#define TEST_RADIO_SCHEDULER_SUPPORT_H

#include <stddef.h>
#include <stdint.h>

#include "ble_radio.h"

typedef enum test_radio_failure_location {
    TEST_RADIO_FAILURE_NONE = 0,
    TEST_RADIO_FAILURE_PRE_DISABLE,
    TEST_RADIO_FAILURE_CH37,
    TEST_RADIO_FAILURE_CH38,
    TEST_RADIO_FAILURE_CH39,
    TEST_RADIO_FAILURE_RESTORE,
} test_radio_failure_location_t;

typedef void (*test_radio_tx_start_handler_t)(void *context);

void test_radio_script_reset(void);
void test_radio_set_tx_start_handler(test_radio_tx_start_handler_t handler,
                                    void *context);
void test_radio_script_tx(uint8_t requested_channel_mask,
                           uint8_t completed_channel_mask,
                           ble_radio_op_result_t fault,
                           test_radio_failure_location_t failure_location);
/* Selects the one-based occurrence of the scripted failing channel. */
void test_radio_script_tx_repeat_failure(
    uint8_t failure_sweep,
    test_radio_failure_location_t failure_location);
void test_radio_script_rx(const uint8_t *raw_pdu, size_t raw_pdu_len,
                           uint8_t rssi_magnitude_db,
                           ble_radio_op_result_t result);
const uint8_t *test_radio_channel_trace(size_t *length_out);
unsigned int test_radio_disable_count(void);

#endif /* TEST_RADIO_SCHEDULER_SUPPORT_H */
