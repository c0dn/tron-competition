#include "tron_timer_config.h"

#include <stdint.h>
#include <stdio.h>

/*
 * This staged BEARER-05/LINK-04/MAINT-03 contract exercises the production
 * frozen host timer object and its production validation function.  It does
 * not provide a substitute timer backend or duplicate validation logic.
 *
 * Required future API:
 *   TRON_TIMER_FUTURE_BEARER_API == 1
 *   tron_timer_config_t adds immutable
 *     radio_tx_repeated_event_bound_ms and
 *     radio_tx_fault_cleanup_bound_ms
 *   tron_timer_config_is_valid() validates every derived bearer bound and the
 *   absolute DATA-deadline half-range rule.
 */
#if !defined(TRON_TIMER_FUTURE_BEARER_API)
#error "TIMER_RED_GATE: TRON_TIMER_FUTURE_BEARER_API=1 is required"
#elif TRON_TIMER_FUTURE_BEARER_API != 1
#error "TIMER_RED_GATE: TRON_TIMER_FUTURE_BEARER_API must equal 1"
#endif

#if defined(TRON_TIMER_FUTURE_BEARER_API) && \
    TRON_TIMER_FUTURE_BEARER_API == 1

static unsigned int failures;

#define CHECK(requirement, expression) \
    do { \
        if (!(expression)) { \
            printf("FAIL %s: %s:%d: assertion failed: %s\n", \
                   (requirement), __FILE__, __LINE__, #expression); \
            failures++; \
        } \
    } while (0)

static void test_balanced_fixture_values_and_formulas(void)
{
    const tron_timer_config_t *config = &tron_timer_config;

    CHECK("BEARER-05", config->radio_state_timeout_ms == 2u &&
                           config->radio_tx_event_bound_ms == 8u &&
                           config->radio_tx_repeated_event_bound_ms == 14u &&
                           config->radio_tx_fault_cleanup_bound_ms == 18u &&
                           config->scheduler_poll_max_ms == 2u &&
                           config->scheduler_custody_bypass_max == 2u);
    CHECK("LINK-04", config->link_hack_timeout_ms == 250u &&
                        config->link_max_attempts == 3u &&
                        config->link_tx_scheduler_attempt_bound_ms == 3048u &&
                        config->link_response_window_sum_ms == 750u &&
                        config->link_no_response_wall_bound_ms == 9894u &&
                        config->link_data_deadline_ms == 5000u);

    CHECK("BEARER-05", config->radio_tx_event_bound_ms ==
                           (1u + 3u) * config->radio_state_timeout_ms);
    CHECK("BEARER-05", config->radio_tx_repeated_event_bound_ms ==
                           (1u + 6u) * config->radio_state_timeout_ms);
    CHECK("BEARER-05", config->radio_tx_fault_cleanup_bound_ms ==
                           (1u + 6u + 2u) * config->radio_state_timeout_ms);
    CHECK("LINK-04", config->link_tx_scheduler_attempt_bound_ms ==
                        (config->scheduler_custody_bypass_max + 1u) *
                            (1000u + config->radio_tx_repeated_event_bound_ms +
                             config->scheduler_poll_max_ms));
    CHECK("LINK-04", config->link_response_window_sum_ms ==
                        config->link_max_attempts * config->link_hack_timeout_ms);
    CHECK("LINK-04", config->link_no_response_wall_bound_ms ==
                        config->link_response_window_sum_ms +
                            config->link_max_attempts *
                                config->link_tx_scheduler_attempt_bound_ms);
    CHECK("LINK-04", config->link_data_deadline_ms <
                        config->link_no_response_wall_bound_ms &&
                        config->link_max_attempts == 3u);
}

static void test_validation_rejects_independent_derived_mutations(void)
{
    tron_timer_config_t mutated = tron_timer_config;

    mutated.radio_tx_event_bound_ms++;
    CHECK("MAINT-03", !tron_timer_config_is_valid(&mutated));

    mutated = tron_timer_config;
    mutated.radio_tx_repeated_event_bound_ms++;
    CHECK("MAINT-03", !tron_timer_config_is_valid(&mutated));

    mutated = tron_timer_config;
    mutated.radio_tx_fault_cleanup_bound_ms++;
    CHECK("MAINT-03", !tron_timer_config_is_valid(&mutated));

    mutated = tron_timer_config;
    mutated.link_tx_scheduler_attempt_bound_ms++;
    CHECK("MAINT-03", !tron_timer_config_is_valid(&mutated));

    mutated = tron_timer_config;
    mutated.link_response_window_sum_ms++;
    CHECK("MAINT-03", !tron_timer_config_is_valid(&mutated));

    mutated = tron_timer_config;
    mutated.link_no_response_wall_bound_ms++;
    CHECK("MAINT-03", !tron_timer_config_is_valid(&mutated));
}

static void test_validation_rejects_absolute_deadline_half_range(void)
{
    tron_timer_config_t mutated = tron_timer_config;

    CHECK("MAINT-03", tron_timer_config_is_valid(&mutated));

    mutated.link_data_deadline_ms = 0x80000000UL;
    CHECK("MAINT-03", !tron_timer_config_is_valid(&mutated));

    mutated = tron_timer_config;
    mutated.link_data_deadline_ms = 0xffffffffUL;
    CHECK("MAINT-03", !tron_timer_config_is_valid(&mutated));
}

int main(void)
{
    test_balanced_fixture_values_and_formulas();
    test_validation_rejects_independent_derived_mutations();
    test_validation_rejects_absolute_deadline_half_range();

    if (failures != 0u) {
        printf("tavrn_future_bearer timer RED tests failed: %u assertion(s)\n",
               failures);
        return 1;
    }
    printf("tavrn_future_bearer timer tests passed\n");
    return 0;
}

#endif /* TRON_TIMER_FUTURE_BEARER_API */
