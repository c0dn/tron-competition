#include "mind_log_formatter.h"

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

typedef struct captured_line {
    char text[MIND_LOG_FORMAT_LINE_BYTES];
    uint8_t calls;
} captured_line_t;

static void capture(void *context, const char *line)
{
    captured_line_t *captured = context;

    if (captured != NULL && line != NULL) {
        captured->calls++;
        strncpy(captured->text, line, sizeof(captured->text) - 1u);
        captured->text[sizeof(captured->text) - 1u] = '\0';
    }
}

static void test_command_and_root_lines(void)
{
    mind_log_record_t record;
    char line[MIND_LOG_FORMAT_LINE_BYTES];

    memset(&record, 0, sizeof(record));
    record.kind = MIND_LOG_COMMAND;
    record.now_ms = 12u;
    record.local_adva = identity(0x80u);
    record.detail.command.command = MIND_COMMAND_ON;
    record.detail.command.status = MIND_COMMAND_ACCEPTED;
    check(mind_log_format_record(&record, line, sizeof(line)) &&
              strcmp(line, "mind_command_v1 now=12 local=8081545678c0 command=on status=accepted\n") == 0,
          "command formatter preserves the exact production UART grammar");

    memset(&record, 0, sizeof(record));
    record.kind = MIND_LOG_ROOT;
    record.now_ms = 99u;
    record.local_adva = identity(0x80u);
    record.detail.root.node_number = 6u;
    record.detail.root.local_active = 1u;
    record.detail.root.state.active_roots = 16u;
    record.detail.root.state.announced = 2u;
    record.detail.root.state.acked = 1u;
    record.detail.root.state.rejected = 3u;
    record.detail.root.state.pending = 4u;
    record.detail.root.rootless_drop = 7u;
    check(mind_log_format_record(&record, line, sizeof(line)) &&
              strcmp(line, "mind_root_v1 now=99 local=8081545678c0 node=6 role=root roots=16 announced=2 acked=1 rejected=3 pending=4 rootless_drop=7\n") == 0,
          "root formatter preserves field order and all-root count");
}

static void test_event_lines_and_injected_sink(void)
{
    mind_log_record_t record;
    captured_line_t captured;
    char line[MIND_LOG_FORMAT_LINE_BYTES];

    memset(&record, 0, sizeof(record));
    record.kind = MIND_LOG_EVENT;
    record.now_ms = 55u;
    record.local_adva = identity(0x80u);
    record.detail.event.wearable = 7u;
    record.detail.event.report.packet_id24 = 0xa1b2c3u;
    record.detail.event.report.schema_payload[0] = 1u;
    record.detail.event.report.schema_payload[1] = MIND_EVT_FALL_AND_SHOUT;
    record.detail.event.report.schema_payload[2] = 73u;
    record.detail.event.report.schema_payload[3] = 0x1fu;
    record.detail.event.report.schema_payload[4] = 0x1au;
    record.detail.event.report.schema_payload[5] = 0x7du;
    record.detail.event.report.schema_payload[6] = 0xc3u;
    record.detail.event.observer = identity(1u);
    check(mind_log_format_record(&record, line, sizeof(line)) &&
              strcmp(line, "mind_event_v1 now=55 root=8081545678c0 wearable=7 packet=a1b2c3 schema=1 event=5 confidence=73 svm=6687 mic=125 seq=195 observer=0102545678c0 path=tavrn\n") == 0,
          "event formatter emits the exact validated routed record");
    check(!mind_log_record_is_heartbeat(&record),
          "incident record remains visible to quiet production telemetry");
    memset(&captured, 0, sizeof(captured));
    mind_log_emit(&record, capture, &captured);
    check(captured.calls == 1u && strcmp(captured.text, line) == 0,
          "injected UART sink receives the same tested event line");

    record.detail.event.observer = identity(2u);
    record.detail.event.path_local = 1u;
    check(mind_log_format_record(&record, line, sizeof(line)) &&
              strcmp(line, "mind_event_v1 now=55 root=8081545678c0 wearable=7 packet=a1b2c3 schema=1 event=5 confidence=73 svm=6687 mic=125 seq=195 observer=0203545678c0 path=local\n") == 0,
          "same dashboard key retains a distinct observer/path record");
    record.detail.event.report.schema_payload[1] = MIND_EVT_HEARTBEAT;
    record.detail.event.report.schema_payload[2] = 0u;
    record.detail.event.report.schema_payload[5] = 0u;
    check(mind_log_record_is_heartbeat(&record),
          "quiet production can identify heartbeat records without formatting");
    record.kind = MIND_LOG_COMMAND;
    check(!mind_log_record_is_heartbeat(&record) &&
              !mind_log_record_is_heartbeat(NULL),
          "heartbeat classification preserves command and null records");
}

int main(void)
{
    test_command_and_root_lines();
    test_event_lines_and_injected_sink();
    if (failures != 0u) {
        printf("mind_log_formatter failures=%u\n", failures);
        return 1;
    }
    printf("mind_log_formatter tests passed\n");
    return 0;
}
