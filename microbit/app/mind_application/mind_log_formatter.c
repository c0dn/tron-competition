#include "mind_log_formatter.h"

#include <string.h>

typedef struct mind_log_writer {
    char *cursor;
    uint16_t remaining;
    uint8_t failed;
} mind_log_writer_t;

static void put_char(mind_log_writer_t *writer, char value)
{
    if (writer == NULL || writer->remaining <= 1u) {
        if (writer != NULL) {
            writer->failed = 1u;
        }
        return;
    }
    *writer->cursor++ = value;
    writer->remaining--;
}

static void put_text(mind_log_writer_t *writer, const char *text)
{
    if (text == NULL) {
        if (writer != NULL) {
            writer->failed = 1u;
        }
        return;
    }
    while (*text != '\0') {
        put_char(writer, *text++);
    }
}

static void put_u32(mind_log_writer_t *writer, uint32_t value)
{
    char reversed[10];
    uint8_t count = 0u;

    do {
        reversed[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    while (count != 0u) {
        put_char(writer, reversed[--count]);
    }
}

static void put_hex_byte(mind_log_writer_t *writer, uint8_t value)
{
    static const char hex[] = "0123456789abcdef";

    put_char(writer, hex[(value >> 4) & 0x0fu]);
    put_char(writer, hex[value & 0x0fu]);
}

static void put_adva(mind_log_writer_t *writer, const tavrn_adva_t *adva)
{
    uint8_t index;

    if (adva == NULL) {
        if (writer != NULL) {
            writer->failed = 1u;
        }
        return;
    }
    for (index = 0u; index < TAVRN_ADVA_LEN; index++) {
        put_hex_byte(writer, adva->bytes[index]);
    }
}

static void put_u24_hex(mind_log_writer_t *writer, uint32_t value)
{
    put_hex_byte(writer, (uint8_t)((value >> 16) & 0xffu));
    put_hex_byte(writer, (uint8_t)((value >> 8) & 0xffu));
    put_hex_byte(writer, (uint8_t)(value & 0xffu));
}

static int format_command(const mind_log_record_t *record, mind_log_writer_t *writer)
{
    put_text(writer, "mind_command_v1 now=");
    put_u32(writer, record->now_ms);
    put_text(writer, " local=");
    put_adva(writer, &record->local_adva);
    put_text(writer, " command=");
    put_text(writer, mind_command_kind_name(record->detail.command.command));
    put_text(writer, " status=");
    put_text(writer, mind_command_status_name(record->detail.command.status));
    put_char(writer, '\n');
    return writer->failed == 0u;
}

static int format_root(const mind_log_record_t *record, mind_log_writer_t *writer)
{
    const mind_root_plane_status_t *state = &record->detail.root.state;

    put_text(writer, "mind_root_v1 now=");
    put_u32(writer, record->now_ms);
    put_text(writer, " local=");
    put_adva(writer, &record->local_adva);
    put_text(writer, " node=");
    put_u32(writer, record->detail.root.node_number);
    put_text(writer, " role=");
    put_text(writer, record->detail.root.local_active != 0u ? "root" : "leaf");
    put_text(writer, " roots=");
    put_u32(writer, state->active_roots);
    put_text(writer, " announced=");
    put_u32(writer, state->announced);
    put_text(writer, " acked=");
    put_u32(writer, state->acked);
    put_text(writer, " rejected=");
    put_u32(writer, state->rejected);
    put_text(writer, " pending=");
    put_u32(writer, state->pending);
    put_text(writer, " rootless_drop=");
    put_u32(writer, record->detail.root.rootless_drop);
    put_char(writer, '\n');
    return writer->failed == 0u;
}

static int format_event(const mind_log_record_t *record, mind_log_writer_t *writer)
{
    const mind_application_report_t *report = &record->detail.event.report;
    const uint8_t *payload = report->schema_payload;
    uint16_t svm = (uint16_t)payload[3] | ((uint16_t)payload[4] << 8);
    uint8_t has_observer_rssi = report->rssi_magnitude_db != 0u &&
        report->rssi_magnitude_db <= 127u;

    put_text(writer, has_observer_rssi != 0u ? "mind_event_v2 now=" :
             "mind_event_v1 now=");
    put_u32(writer, record->now_ms);
    put_text(writer, " root=");
    put_adva(writer, &record->local_adva);
    put_text(writer, " wearable=");
    put_u32(writer, record->detail.event.wearable);
    put_text(writer, " packet=");
    put_u24_hex(writer, report->packet_id24);
    put_text(writer, " schema=");
    put_u32(writer, payload[0]);
    put_text(writer, " event=");
    put_u32(writer, payload[1]);
    put_text(writer, " confidence=");
    put_u32(writer, payload[2]);
    put_text(writer, " svm=");
    put_u32(writer, svm);
    put_text(writer, " mic=");
    put_u32(writer, payload[5]);
    put_text(writer, " seq=");
    put_u32(writer, payload[6]);
    put_text(writer, " observer=");
    put_adva(writer, &record->detail.event.observer);
    if (has_observer_rssi != 0u) {
        put_text(writer, " observer_rssi_dbm=-");
        put_u32(writer, report->rssi_magnitude_db);
    }
    put_text(writer, " path=");
    put_text(writer, record->detail.event.path_local != 0u ? "local" : "tavrn");
    put_char(writer, '\n');
    return writer->failed == 0u;
}

int mind_log_format_record(const mind_log_record_t *record, char *line_out,
                           uint16_t line_capacity)
{
    mind_log_writer_t writer;
    int result;

    if (record == NULL || line_out == NULL || line_capacity == 0u) {
        return 0;
    }
    memset(line_out, 0, line_capacity);
    writer.cursor = line_out;
    writer.remaining = line_capacity;
    writer.failed = 0u;
    if (record->kind == MIND_LOG_COMMAND) {
        result = format_command(record, &writer);
    } else if (record->kind == MIND_LOG_ROOT) {
        result = format_root(record, &writer);
    } else if (record->kind == MIND_LOG_EVENT) {
        result = format_event(record, &writer);
    } else {
        return 0;
    }
    if (writer.failed != 0u || result == 0) {
        line_out[0] = '\0';
        return 0;
    }
    *writer.cursor = '\0';
    return 1;
}

void mind_log_emit(const mind_log_record_t *record, mind_log_line_sink_fn sink,
                   void *context)
{
    char line[MIND_LOG_FORMAT_LINE_BYTES];

    if (sink != NULL && mind_log_format_record(record, line, sizeof(line))) {
        sink(context, line);
    }
}
