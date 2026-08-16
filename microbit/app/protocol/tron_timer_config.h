#ifndef TRON_TIMER_CONFIG_H
#define TRON_TIMER_CONFIG_H

#include <stdint.h>

#define TRON_TIMER_FUTURE_BEARER_API 1

/*
 * The selected profile is represented by exactly one immutable object.  Timer
 * consumers receive a pointer to this value when they initialize; algorithm
 * code must not select a profile or supply its own behavioural defaults.
 */
typedef struct tron_timer_config {
    uint32_t scheduler_dwell_ms;
    uint32_t scheduler_relay_spacing_ms;
    uint32_t scheduler_custody_bypass_max;
    uint32_t scheduler_poll_max_ms;
    uint32_t radio_state_timeout_ms;
    uint32_t radio_tx_event_bound_ms;
    uint32_t radio_tx_repeated_event_bound_ms;
    uint32_t radio_tx_fault_cleanup_bound_ms;
    uint32_t legacy_relay_min_ms;
    uint32_t legacy_relay_max_ms;
    uint32_t legacy_dedupe_ms;
    uint32_t legacy_ping_interval_ms;
    uint32_t legacy_ping_timeout_ms;
    uint32_t link_hack_timeout_ms;
    uint32_t link_max_attempts;
    uint32_t link_retry_backoff_ms;
    uint32_t link_tx_scheduler_attempt_bound_ms;
    uint32_t link_response_window_sum_ms;
    uint32_t link_no_response_wall_bound_ms;
    uint32_t link_candidate_resolve_ms;
    uint32_t link_busy_backoff_ms;
    uint32_t link_busy_max_responses;
    uint32_t link_data_deadline_ms;
    uint32_t link_data_dedupe_ms;
    uint32_t link_flood_dedupe_ms;
    uint32_t link_flood_jitter_min_ms;
    uint32_t link_flood_jitter_max_ms;
    uint32_t aodv_node_traversal_ms;
    uint32_t aodv_net_diameter;
    uint32_t aodv_net_traversal_ms;
    uint32_t aodv_path_discovery_ms;
    uint32_t aodv_rreq_seen_ms;
    uint32_t aodv_rreq_retries;
    uint32_t aodv_rreq_rate;
    uint32_t aodv_rerr_rate;
    uint32_t aodv_active_route_ms;
    uint32_t aodv_pending_data_ms;
    uint32_t aodv_blacklist_ms;
    uint32_t aodv_rrep_dedupe_ms;
    uint32_t aodv_rerr_dedupe_ms;
    uint32_t aodv_rrep_ack_wait_ms;
    uint32_t gtt_soft_expiry_ms;
    uint32_t gtt_hard_expiry_ms;
    uint32_t gtt_departed_ms;
    uint32_t gtt_maintenance_ms;
    uint32_t hello_change_ms;
    uint32_t hello_stable_ms;
    float hello_alpha;
    float hello_snap_ratio;
    uint32_t hello_dedupe_ms;
    uint32_t router_reboot_announce_ms;
    uint32_t freshness_response_min_ms;
    uint32_t freshness_response_max_ms;
    uint32_t verification_window_ms;
    uint32_t verification_new_cap;
    uint32_t verification_active_cap;
    uint32_t mentor_offer_window_ms;
    uint32_t mentor_page_timeout_ms;
    uint32_t mentor_page_attempts;
    uint32_t mentor_self_bootstrap_ms;
    uint32_t mentor_sync_dedupe_ms;
    uint32_t mentor_rssi_weak_magnitude_db;
    uint32_t mentor_rssi_strong_magnitude_db;
    uint32_t mentor_rssi_weak_delay_ms;
    uint32_t mentor_rssi_strong_delay_ms;
    uint32_t mentor_jitter_min_ms;
    uint32_t mentor_jitter_max_ms;
    uint32_t mentor_offer_suppression_ms;
    uint32_t metadata_cooldown_ms;
    uint32_t tc_uuid_ms;
    uint32_t tc_subject_ms;
    uint32_t repair_timeout_ms;
    uint32_t repair_cooldown_ms;
    uint32_t stats_ms;
    uint32_t loop_delay_ms;
} tron_timer_config_t;

/* The firmware definition is generated from TRON_TIMER_PROFILE.  Host runners
 * link the frozen balanced definition in tron_timer_config.c instead. */
extern const tron_timer_config_t tron_timer_config;

static inline int tron_timer_config_is_valid(const tron_timer_config_t *config)
{
    uint64_t expected_radio_tx_event_bound_ms;
    uint64_t expected_radio_tx_repeated_event_bound_ms;
    uint64_t expected_radio_tx_fault_cleanup_bound_ms;
    uint64_t expected_link_tx_scheduler_attempt_bound_ms;
    uint64_t expected_link_response_window_sum_ms;
    uint64_t expected_link_no_response_wall_bound_ms;
    uint64_t expected_verification_window_ms;

    if (config == 0 || config->scheduler_dwell_ms == 0u ||
        config->scheduler_relay_spacing_ms == 0u ||
        config->scheduler_custody_bypass_max == 0u ||
        config->scheduler_poll_max_ms == 0u ||
        config->radio_state_timeout_ms == 0u ||
        config->link_hack_timeout_ms == 0u ||
        config->link_max_attempts == 0u ||
        config->link_candidate_resolve_ms == 0u ||
        config->link_busy_max_responses == 0u ||
        config->link_data_deadline_ms == 0u ||
        config->legacy_dedupe_ms == 0u ||
        config->legacy_ping_interval_ms == 0u ||
        config->legacy_ping_timeout_ms == 0u ||
        config->legacy_ping_timeout_ms >= config->legacy_ping_interval_ms ||
        config->legacy_relay_min_ms > config->legacy_relay_max_ms ||
        config->link_flood_jitter_min_ms > config->link_flood_jitter_max_ms ||
        config->mentor_jitter_min_ms > config->mentor_jitter_max_ms ||
        config->freshness_response_min_ms == 0u ||
        config->freshness_response_max_ms == 0u ||
        config->freshness_response_min_ms > config->freshness_response_max_ms ||
        config->verification_window_ms == 0u ||
        config->hello_alpha <= 0.0f || config->hello_alpha > 1.0f ||
        config->hello_snap_ratio <= 0.0f || config->hello_snap_ratio > 1.0f) {
        return 0;
    }
    /* Check every bearer-formula input before evaluating it so the uint64_t
     * products below remain exact for every accepted configuration. */
    if (config->scheduler_custody_bypass_max >= 0x80000000UL ||
        config->scheduler_poll_max_ms >= 0x80000000UL ||
        config->radio_state_timeout_ms >= 0x80000000UL ||
        config->radio_tx_event_bound_ms >= 0x80000000UL ||
        config->radio_tx_repeated_event_bound_ms >= 0x80000000UL ||
        config->radio_tx_fault_cleanup_bound_ms >= 0x80000000UL ||
        config->link_hack_timeout_ms >= 0x80000000UL ||
        config->link_max_attempts >= 0x80000000UL ||
        config->link_tx_scheduler_attempt_bound_ms >= 0x80000000UL ||
        config->link_response_window_sum_ms >= 0x80000000UL ||
        config->link_no_response_wall_bound_ms >= 0x80000000UL ||
        config->link_data_deadline_ms >= 0x80000000UL) {
        return 0;
    }
    expected_radio_tx_event_bound_ms =
        (uint64_t)(1u + 3u) * (uint64_t)config->radio_state_timeout_ms;
    expected_radio_tx_repeated_event_bound_ms =
        (uint64_t)(1u + 6u) * (uint64_t)config->radio_state_timeout_ms;
    expected_radio_tx_fault_cleanup_bound_ms =
        (uint64_t)(1u + 6u + 2u) * (uint64_t)config->radio_state_timeout_ms;
    expected_link_tx_scheduler_attempt_bound_ms =
        ((uint64_t)config->scheduler_custody_bypass_max + 1u) *
        (1000u + (uint64_t)config->radio_tx_repeated_event_bound_ms +
         (uint64_t)config->scheduler_poll_max_ms);
    expected_link_response_window_sum_ms =
        (uint64_t)config->link_max_attempts *
        (uint64_t)config->link_hack_timeout_ms;
    expected_link_no_response_wall_bound_ms =
        (uint64_t)config->link_response_window_sum_ms +
        (uint64_t)config->link_max_attempts *
        (uint64_t)config->link_tx_scheduler_attempt_bound_ms;
    expected_verification_window_ms =
        (uint64_t)config->aodv_net_traversal_ms +
        2u * (uint64_t)config->aodv_path_discovery_ms;
    if (expected_radio_tx_event_bound_ms >= 0x80000000ULL ||
        expected_radio_tx_repeated_event_bound_ms >= 0x80000000ULL ||
        expected_radio_tx_fault_cleanup_bound_ms >= 0x80000000ULL ||
        expected_link_tx_scheduler_attempt_bound_ms >= 0x80000000ULL ||
        expected_link_response_window_sum_ms >= 0x80000000ULL ||
        expected_link_no_response_wall_bound_ms >= 0x80000000ULL ||
        expected_radio_tx_event_bound_ms !=
            (uint64_t)config->radio_tx_event_bound_ms ||
        expected_radio_tx_repeated_event_bound_ms !=
            (uint64_t)config->radio_tx_repeated_event_bound_ms ||
        expected_radio_tx_fault_cleanup_bound_ms !=
            (uint64_t)config->radio_tx_fault_cleanup_bound_ms ||
        expected_link_tx_scheduler_attempt_bound_ms !=
            (uint64_t)config->link_tx_scheduler_attempt_bound_ms ||
        expected_link_response_window_sum_ms !=
            (uint64_t)config->link_response_window_sum_ms ||
        expected_link_no_response_wall_bound_ms !=
            (uint64_t)config->link_no_response_wall_bound_ms ||
        config->link_data_deadline_ms >=
            config->link_no_response_wall_bound_ms ||
        config->verification_window_ms >= 0x80000000UL ||
        expected_verification_window_ms >= 0x80000000ULL ||
        expected_verification_window_ms !=
            (uint64_t)config->verification_window_ms) {
        return 0;
    }
    return config->scheduler_dwell_ms < 0x80000000UL &&
        config->scheduler_relay_spacing_ms < 0x80000000UL &&
        config->legacy_relay_max_ms < 0x80000000UL &&
        config->legacy_dedupe_ms < 0x80000000UL &&
        config->legacy_ping_interval_ms < 0x80000000UL &&
        config->legacy_ping_timeout_ms < 0x80000000UL &&
        config->link_retry_backoff_ms < 0x80000000UL &&
        config->freshness_response_min_ms < 0x80000000UL &&
        config->freshness_response_max_ms < 0x80000000UL &&
        config->stats_ms < 0x80000000UL &&
        config->loop_delay_ms < 0x80000000UL;
}

#endif /* TRON_TIMER_CONFIG_H */
