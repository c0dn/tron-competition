#include "mind_gtt_response.h"
#include "mind_log.h"

#include <stdio.h>
#include <string.h>

static unsigned failures;

static void check(int condition, const char *message)
{
    if (!condition) {
        failures++;
        printf("FAIL: %s\n", message);
    }
}

static tavrn_adva_t identity(uint8_t value)
{
    tavrn_adva_t result = { { value, (uint8_t)(value + 1u), 0x54u, 0x56u,
                               0x78u, 0xc0u } };
    return result;
}

static routed_cycle_gtt_snapshot_t snapshot(uint32_t query_at_ms,
                                             uint8_t entry_count)
{
    routed_cycle_gtt_snapshot_t result;
    uint8_t index;

    memset(&result, 0, sizeof(result));
    result.query_at_ms = query_at_ms;
    result.local_canonical_adva = identity(0x80u);
    result.entry_count = entry_count;
    result.nondeparted_count = entry_count;
    for (index = 0u; index < entry_count; index++) {
        routed_cycle_gtt_snapshot_entry_t *entry = &result.entries[index];

        entry->canonical_adva = identity((uint8_t)(index + 1u));
        entry->last_evidence_ms = 10u + index;
        entry->soft_deadline_ms = 20u + index;
        entry->hard_deadline_ms = 30u + index;
        entry->departed_deadline_ms = 40u + index;
        entry->serial = (uint16_t)(4660u + index);
        entry->serial_state = ROUTED_CYCLE_VALUE_KNOWN;
        entry->hop_count = 2u;
        entry->hop_state = ROUTED_CYCLE_VALUE_UNKNOWN;
        entry->freshness = ROUTED_CYCLE_GTT_FRESHNESS_HARD_EXPIRED;
        entry->departed = ROUTED_CYCLE_BOOLEAN_FALSE;
    }
    return result;
}

static int is_one_line(const char *line)
{
    size_t length;

    if (line == NULL || line[0] == '\0') {
        return 0;
    }
    length = strlen(line);
    return length != 0u && line[length - 1u] == '\n' &&
        strchr(line, '\n') == line + length - 1u;
}

static void ready_response(mind_gtt_response_t *response)
{
    (void)mind_gtt_response_claim(response);
    mind_gtt_response_commit(response);
    mind_gtt_response_snapshot_ready(response);
}

static void test_request_states_and_snapshot_failure(void)
{
    mind_gtt_response_t response;
    routed_cycle_gtt_snapshot_t copied = snapshot(5u, 0u);
    char line[MIND_GTT_RESPONSE_LINE_BYTES];

    mind_gtt_response_init(&response);
    check(response.state == MIND_GTT_RESPONSE_IDLE &&
          !mind_gtt_response_is_busy(&response) &&
          mind_gtt_response_claim(&response) &&
          response.state == MIND_GTT_RESPONSE_CLAIMED &&
          mind_gtt_response_is_busy(&response) &&
          !mind_gtt_response_claim(&response),
          "only idle owns the single GTT request slot");
    mind_gtt_response_cancel(&response);
    check(response.state == MIND_GTT_RESPONSE_IDLE &&
          !mind_gtt_response_is_busy(&response),
          "cancel returns a claimed GTT request to idle");

    check(mind_gtt_response_claim(&response), "pending-state fixture claims GTT");
    mind_gtt_response_commit(&response);
    check(response.state == MIND_GTT_RESPONSE_PENDING &&
          mind_gtt_response_is_busy(&response) &&
          !mind_gtt_response_format_next(&response, &copied, line, sizeof(line)),
          "pending GTT is busy and cannot emit an internal snapshot");
    mind_gtt_response_snapshot_failed(&response);
    check(response.state == MIND_GTT_RESPONSE_IDLE &&
          !mind_gtt_response_is_busy(&response) &&
          !mind_gtt_response_format_next(&response, &copied, line, sizeof(line)),
          "snapshot failure releases the UI request without output");

    ready_response(&response);
    check(response.state == MIND_GTT_RESPONSE_READY &&
          mind_gtt_response_is_busy(&response) &&
          mind_gtt_response_format_next(&response, &copied, line, sizeof(line)) &&
          response.state == MIND_GTT_RESPONSE_EMITTING &&
          mind_gtt_response_is_busy(&response),
          "ready and emitting GTT states both retain the sole request slot");
}

static void test_zero_entry_response(void)
{
    mind_gtt_response_t response;
    routed_cycle_gtt_snapshot_t copied = snapshot(7u, 0u);
    char line[MIND_GTT_RESPONSE_LINE_BYTES];

    mind_gtt_response_init(&response);
    ready_response(&response);
    check(mind_gtt_response_format_next(&response, &copied, line, sizeof(line)) &&
          is_one_line(line) &&
          strcmp(line,
                 "mind_gtt_begin_v1 query=7 local=8081545678c0 entries=0 nondeparted=0\n") ==
              0,
          "zero-entry GTT emits its exact begin line");
    check(mind_gtt_response_format_next(&response, &copied, line, sizeof(line)) &&
          is_one_line(line) &&
          strcmp(line,
                 "mind_gtt_end_v1 query=7 local=8081545678c0 entries=0 nondeparted=0\n") ==
              0 &&
          response.state == MIND_GTT_RESPONSE_IDLE &&
          !mind_gtt_response_is_busy(&response),
          "zero-entry GTT always emits its exact end line and releases storage");
    check(!mind_gtt_response_format_next(&response, &copied, line, sizeof(line)),
          "zero-entry response emits no second line in the completed turn");
}

static void test_sixteen_entry_response(void)
{
    mind_gtt_response_t response;
    routed_cycle_gtt_snapshot_t copied = snapshot(77u, 16u);
    char line[MIND_GTT_RESPONSE_LINE_BYTES];
    uint8_t index;

    mind_gtt_response_init(&response);
    ready_response(&response);
    check(mind_gtt_response_format_next(&response, &copied, line, sizeof(line)) &&
          is_one_line(line) &&
          strcmp(line,
                 "mind_gtt_begin_v1 query=77 local=8081545678c0 entries=16 nondeparted=16\n") ==
              0,
          "16-entry GTT emits its exact bounded begin line");
    for (index = 0u; index < 16u; index++) {
        char prefix[40];
        int prefix_length = snprintf(prefix, sizeof(prefix),
                                     "mind_gtt_entry_v1 query=77 index=%u ",
                                     (unsigned int)index);

        check(prefix_length > 0 && (uint16_t)prefix_length < sizeof(prefix) &&
              mind_gtt_response_format_next(&response, &copied, line, sizeof(line)) &&
              is_one_line(line) && strncmp(line, prefix, (size_t)prefix_length) == 0,
              "16-entry GTT emits one correctly indexed entry per call");
        if (index == 0u) {
            check(strcmp(line,
                         "mind_gtt_entry_v1 query=77 index=0 adva=0102545678c0 last=10 soft=20 hard=30 departed_deadline=40 serial=4660 serial_state=1 hop=2 hop_state=2 freshness=3 departed=1\n") ==
                      0,
                  "GTT entry uses lowercase AdvA hex and unchanged numeric enum values");
        }
    }
    check(mind_gtt_response_format_next(&response, &copied, line, sizeof(line)) &&
          is_one_line(line) &&
          strcmp(line,
                 "mind_gtt_end_v1 query=77 local=8081545678c0 entries=16 nondeparted=16\n") ==
              0 &&
          response.state == MIND_GTT_RESPONSE_IDLE,
          "16-entry GTT emits one final end line after all 16 entries");
}

static int logger_turn(mind_log_queue_t *logs, mind_gtt_response_t *response,
                       const routed_cycle_gtt_snapshot_t *copied,
                       char *line_out, uint16_t line_capacity)
{
    mind_log_record_t record;

    if (line_out != NULL && line_capacity != 0u) {
        line_out[0] = '\0';
    }
    if (mind_log_queue_take(logs, &record)) {
        return 1;
    }
    return mind_gtt_response_format_next(response, copied, line_out, line_capacity) ? 2 : 0;
}

static void test_mind_priority_and_snapshot_isolation(void)
{
    mind_gtt_response_t response;
    mind_log_queue_t logs;
    mind_log_reservation_t reservation;
    mind_log_record_t record;
    routed_cycle_gtt_snapshot_t internal = snapshot(55u, 1u);
    routed_cycle_gtt_snapshot_t ui = snapshot(77u, 1u);
    routed_cycle_gtt_snapshot_t internal_before;
    char line[MIND_GTT_RESPONSE_LINE_BYTES];

    memset(&record, 0, sizeof(record));
    record.kind = MIND_LOG_COMMAND;
    mind_log_queue_init(&logs);
    check(mind_log_queue_reserve(&logs, 1u, &reservation) &&
          mind_log_queue_commit(&logs, &reservation, &record),
          "MIND-priority fixture queues one production record");
    mind_gtt_response_init(&response);
    ready_response(&response);
    internal_before = internal;
    check(logger_turn(&logs, &response, &ui, line, sizeof(line)) == 1 &&
          line[0] == '\0' && response.state == MIND_GTT_RESPONSE_READY,
          "a queued production MIND record outranks ready GTT output");
    check(logger_turn(&logs, &response, &ui, line, sizeof(line)) == 2 &&
          is_one_line(line) &&
          strcmp(line,
                 "mind_gtt_begin_v1 query=77 local=8081545678c0 entries=1 nondeparted=1\n") ==
              0 &&
          memcmp(&internal, &internal_before, sizeof(internal)) == 0 &&
          !mind_gtt_response_claim(&response),
          "UI response begins one line at a time without claiming or changing internal storage");
    check(logger_turn(&logs, &response, &ui, line, sizeof(line)) == 2 &&
          is_one_line(line) && strstr(line, "query=77 index=0") != NULL &&
          logger_turn(&logs, &response, &ui, line, sizeof(line)) == 2 &&
          is_one_line(line) && strstr(line, "mind_gtt_end_v1 query=77") != NULL,
          "ready UI storage remains isolated through one entry and one end turn");
}

static void test_malformed_snapshot_releases_request(void)
{
    mind_gtt_response_t response;
    routed_cycle_gtt_snapshot_t copied = snapshot(9u, 0u);
    char line[MIND_GTT_RESPONSE_LINE_BYTES];

    copied.entry_count = ROUTED_CYCLE_GTT_SNAPSHOT_CAPACITY + 1u;
    mind_gtt_response_init(&response);
    ready_response(&response);
    check(!mind_gtt_response_format_next(&response, &copied, line, sizeof(line)) &&
          line[0] == '\0' && response.state == MIND_GTT_RESPONSE_IDLE &&
          !mind_gtt_response_is_busy(&response) && mind_gtt_response_claim(&response),
          "malformed copied snapshots release the bounded UI request without an out-of-range read");
}

int main(void)
{
    test_request_states_and_snapshot_failure();
    test_zero_entry_response();
    test_sixteen_entry_response();
    test_mind_priority_and_snapshot_isolation();
    test_malformed_snapshot_releases_request();
    if (failures != 0u) {
        printf("mind_gtt_response failures=%u\n", failures);
        return 1;
    }
    printf("mind_gtt_response tests passed\n");
    return 0;
}
