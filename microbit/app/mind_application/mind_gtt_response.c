#include "mind_gtt_response.h"

#include <string.h>

typedef struct mind_gtt_response_writer {
    char *cursor;
    uint16_t remaining;
    uint8_t failed;
} mind_gtt_response_writer_t;

static void put_char(mind_gtt_response_writer_t *writer, char value)
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

static void put_text(mind_gtt_response_writer_t *writer, const char *text)
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

static void put_u32(mind_gtt_response_writer_t *writer, uint32_t value)
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

static void put_hex_byte(mind_gtt_response_writer_t *writer, uint8_t value)
{
    static const char hex[] = "0123456789abcdef";

    put_char(writer, hex[(value >> 4) & 0x0fu]);
    put_char(writer, hex[value & 0x0fu]);
}

static void put_adva(mind_gtt_response_writer_t *writer, const tavrn_adva_t *adva)
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

static int snapshot_is_valid(const routed_cycle_gtt_snapshot_t *snapshot)
{
    return snapshot != NULL &&
        snapshot->entry_count <= ROUTED_CYCLE_GTT_SNAPSHOT_CAPACITY &&
        snapshot->nondeparted_count <= snapshot->entry_count;
}

static int format_begin(const routed_cycle_gtt_snapshot_t *snapshot,
                        mind_gtt_response_writer_t *writer)
{
    put_text(writer, "mind_gtt_begin_v1 query=");
    put_u32(writer, snapshot->query_at_ms);
    put_text(writer, " local=");
    put_adva(writer, &snapshot->local_canonical_adva);
    put_text(writer, " entries=");
    put_u32(writer, snapshot->entry_count);
    put_text(writer, " nondeparted=");
    put_u32(writer, snapshot->nondeparted_count);
    put_char(writer, '\n');
    return writer->failed == 0u;
}

static int format_entry(const routed_cycle_gtt_snapshot_t *snapshot,
                        uint8_t index, mind_gtt_response_writer_t *writer)
{
    const routed_cycle_gtt_snapshot_entry_t *entry = &snapshot->entries[index];

    put_text(writer, "mind_gtt_entry_v1 query=");
    put_u32(writer, snapshot->query_at_ms);
    put_text(writer, " index=");
    put_u32(writer, index);
    put_text(writer, " adva=");
    put_adva(writer, &entry->canonical_adva);
    put_text(writer, " last=");
    put_u32(writer, entry->last_evidence_ms);
    put_text(writer, " soft=");
    put_u32(writer, entry->soft_deadline_ms);
    put_text(writer, " hard=");
    put_u32(writer, entry->hard_deadline_ms);
    put_text(writer, " departed_deadline=");
    put_u32(writer, entry->departed_deadline_ms);
    put_text(writer, " serial=");
    put_u32(writer, entry->serial);
    put_text(writer, " serial_state=");
    put_u32(writer, entry->serial_state);
    put_text(writer, " hop=");
    put_u32(writer, entry->hop_count);
    put_text(writer, " hop_state=");
    put_u32(writer, entry->hop_state);
    put_text(writer, " freshness=");
    put_u32(writer, entry->freshness);
    put_text(writer, " departed=");
    put_u32(writer, entry->departed);
    put_char(writer, '\n');
    return writer->failed == 0u;
}

static int format_end(const routed_cycle_gtt_snapshot_t *snapshot,
                      mind_gtt_response_writer_t *writer)
{
    put_text(writer, "mind_gtt_end_v1 query=");
    put_u32(writer, snapshot->query_at_ms);
    put_text(writer, " local=");
    put_adva(writer, &snapshot->local_canonical_adva);
    put_text(writer, " entries=");
    put_u32(writer, snapshot->entry_count);
    put_text(writer, " nondeparted=");
    put_u32(writer, snapshot->nondeparted_count);
    put_char(writer, '\n');
    return writer->failed == 0u;
}

void mind_gtt_response_init(mind_gtt_response_t *response)
{
    if (response != NULL) {
        memset(response, 0, sizeof(*response));
    }
}

int mind_gtt_response_claim(mind_gtt_response_t *response)
{
    if (response == NULL || response->state != MIND_GTT_RESPONSE_IDLE) {
        return 0;
    }
    response->state = MIND_GTT_RESPONSE_CLAIMED;
    return 1;
}

void mind_gtt_response_commit(mind_gtt_response_t *response)
{
    if (response != NULL && response->state == MIND_GTT_RESPONSE_CLAIMED) {
        response->state = MIND_GTT_RESPONSE_PENDING;
    }
}

void mind_gtt_response_cancel(mind_gtt_response_t *response)
{
    if (response != NULL && response->state == MIND_GTT_RESPONSE_CLAIMED) {
        response->state = MIND_GTT_RESPONSE_IDLE;
        response->entry_cursor = 0u;
    }
}

void mind_gtt_response_snapshot_ready(mind_gtt_response_t *response)
{
    if (response != NULL && response->state == MIND_GTT_RESPONSE_PENDING) {
        response->state = MIND_GTT_RESPONSE_READY;
        response->entry_cursor = 0u;
    }
}

void mind_gtt_response_snapshot_failed(mind_gtt_response_t *response)
{
    if (response != NULL && response->state == MIND_GTT_RESPONSE_PENDING) {
        response->state = MIND_GTT_RESPONSE_IDLE;
        response->entry_cursor = 0u;
    }
}

int mind_gtt_response_is_busy(const mind_gtt_response_t *response)
{
    return response != NULL && response->state != MIND_GTT_RESPONSE_IDLE;
}

int mind_gtt_response_format_next(mind_gtt_response_t *response,
                                  const routed_cycle_gtt_snapshot_t *snapshot,
                                  char *line_out, uint16_t line_capacity)
{
    mind_gtt_response_writer_t writer;
    int formatted;

    if (response == NULL || snapshot == NULL || line_out == NULL || line_capacity == 0u ||
        (response->state != MIND_GTT_RESPONSE_READY &&
         response->state != MIND_GTT_RESPONSE_EMITTING)) {
        return 0;
    }
    memset(line_out, 0, line_capacity);
    if (!snapshot_is_valid(snapshot)) {
        response->state = MIND_GTT_RESPONSE_IDLE;
        response->entry_cursor = 0u;
        return 0;
    }
    writer.cursor = line_out;
    writer.remaining = line_capacity;
    writer.failed = 0u;
    if (response->state == MIND_GTT_RESPONSE_READY) {
        formatted = format_begin(snapshot, &writer);
        if (formatted) {
            response->state = MIND_GTT_RESPONSE_EMITTING;
            response->entry_cursor = 0u;
        }
    } else if (response->entry_cursor < snapshot->entry_count) {
        formatted = format_entry(snapshot, response->entry_cursor, &writer);
        if (formatted) {
            response->entry_cursor++;
        }
    } else {
        formatted = format_end(snapshot, &writer);
        if (formatted) {
            response->state = MIND_GTT_RESPONSE_IDLE;
            response->entry_cursor = 0u;
        }
    }
    if (!formatted || writer.failed != 0u) {
        line_out[0] = '\0';
        return 0;
    }
    *writer.cursor = '\0';
    return 1;
}
