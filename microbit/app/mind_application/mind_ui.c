#include "mind_ui.h"

#include <string.h>

#ifndef MIND_APPLICATION_HOST_TEST
#include <tk/tkernel.h>

#include "display.h"
#include "tron_build_config.h"
#endif

static const uint8_t mind_ui_digits[6][5] = {
    /* Leaf glyphs leave (3,4) and (4,4) for the root-presence overlay. */
    { 0x04u, 0x06u, 0x04u, 0x04u, 0x02u },
    { 0x0eu, 0x11u, 0x08u, 0x04u, 0x07u },
    { 0x0eu, 0x11u, 0x0cu, 0x11u, 0x06u },
    { 0x08u, 0x0cu, 0x0au, 0x1fu, 0x02u },
    { 0x0fu, 0x01u, 0x07u, 0x10u, 0x07u },
    { 0x0eu, 0x01u, 0x07u, 0x11u, 0x06u },
};
static const uint8_t mind_ui_root_glyph[5] = {
    0x0eu, 0x10u, 0x17u, 0x11u, 0x0eu,
};

#ifndef MIND_APPLICATION_HOST_TEST
#if defined(__GNUC__)
#define MIND_UI_TASK_STACK_ALIGNMENT \
    __attribute__((aligned(TRON_BUILD_MIND_TASK_STACK_ALIGNMENT_BYTES)))
#else
#define MIND_UI_TASK_STACK_ALIGNMENT
#endif
typedef char mind_ui_task_stack_size_guard[
    (TRON_BUILD_MIND_UI_TASK_STATIC_BUFFER_BYTES ==
     TRON_BUILD_MIND_UI_TASK_STACK_BYTES +
     TRON_BUILD_MIND_TASK_SYSTEM_STACK_BYTES) ? 1 : -1];
typedef char mind_ui_task_stack_alignment_guard[
    (TRON_BUILD_MIND_UI_TASK_STATIC_BUFFER_BYTES %
     TRON_BUILD_MIND_TASK_STACK_ALIGNMENT_BYTES == 0u) ? 1 : -1];
static uint8_t mind_ui_task_stack[TRON_BUILD_MIND_UI_TASK_STATIC_BUFFER_BYTES]
    MIND_UI_TASK_STACK_ALIGNMENT;
#endif

void mind_ui_init_state(mind_ui_t *ui, uint8_t node_number)
{
    if (ui == NULL || node_number < 1u || node_number > 6u) {
        return;
    }
    memset(ui, 0, sizeof(*ui));
    ui->node_number = node_number;
    ui->initialized = 1u;
    mind_audio_init(&ui->audio);
}

void mind_ui_publish(mind_ui_t *ui, uint8_t local_root, uint8_t active_roots)
{
    if (ui == NULL || ui->initialized == 0u) {
        return;
    }
    local_root = local_root != 0u ? 1u : 0u;
    ui->desired_guard++;
    ui->desired_local_root = local_root;
    ui->desired_active_roots = active_roots;
    if (local_root != 0u && ui->producer_local_root == 0u) {
        ui->desired_transition_sequence++;
        if (ui->desired_transition_sequence == 0u) {
            ui->desired_transition_sequence = 1u;
        }
    }
    ui->producer_local_root = local_root;
    ui->desired_guard++;
}

void mind_ui_service(mind_ui_t *ui, uint32_t now_ms)
{
    uint32_t sequence;
    uint32_t guard_before;
    uint32_t guard_after;
    uint8_t local_root;
    uint8_t active_roots;

    if (ui == NULL || ui->initialized == 0u) {
        return;
    }
    do {
        guard_before = ui->desired_guard;
        if ((guard_before & 1u) != 0u) {
            continue;
        }
        local_root = ui->desired_local_root;
        active_roots = ui->desired_active_roots;
        sequence = ui->desired_transition_sequence;
        guard_after = ui->desired_guard;
    } while (guard_before != guard_after || (guard_after & 1u) != 0u);
    ui->local_root = local_root;
    ui->active_roots = active_roots;
    while (ui->observed_transition_sequence != sequence) {
        mind_audio_queue_root_cue(&ui->audio, now_ms);
        ui->observed_transition_sequence++;
        if (ui->observed_transition_sequence == 0u) {
            ui->observed_transition_sequence = 1u;
        }
    }
    mind_audio_service(&ui->audio, now_ms);
}

void mind_ui_render(const mind_ui_t *ui, uint32_t now_ms, uint8_t rows_out[5])
{
    uint8_t index;

    if (rows_out == NULL) {
        return;
    }
    (void)now_ms;
    memset(rows_out, 0, 5u);
    if (ui == NULL || ui->initialized == 0u) {
        return;
    }
    if (ui->local_root != 0u) {
        memcpy(rows_out, mind_ui_root_glyph, sizeof(mind_ui_root_glyph));
        return;
    }
    memcpy(rows_out, mind_ui_digits[ui->node_number - 1u],
           sizeof(mind_ui_digits[0]));
    if (ui->active_roots == 0u) {
        return;
    }
    rows_out[4] |= 0x10u;
    if (ui->active_roots >= 2u) {
        rows_out[4] |= 0x08u;
    }
    for (index = 0u; index < 5u; index++) {
        rows_out[index] &= 0x1fu;
    }
}

#ifndef MIND_APPLICATION_HOST_TEST
static void mind_ui_task(INT stacd, void *exinf)
{
    mind_ui_t *ui = (mind_ui_t *)exinf;
    uint8_t rows[5];

    (void)stacd;
    mind_audio_hardware_init();
    while (1) {
        SYSTIM system_time;
        uint32_t now;

        tk_get_otm(&system_time);
        now = system_time.lo;
        mind_ui_service(ui, now);
        mind_ui_render(ui, now, rows);
        display_set_rows(rows);
        (void)tk_dly_tsk(10u);
    }
}

int mind_ui_start(mind_ui_t *ui)
{
    T_CTSK task;
    ID id;

    if (ui == NULL || ui->initialized == 0u) {
        return 0;
    }
    if (!display_init_with_priority(MIND_UI_TASK_PRIORITY)) {
        return 0;
    }
    memset(&task, 0, sizeof(task));
    task.exinf = ui;
    task.tskatr = TA_HLNG | TA_RNG3 | TA_USERBUF;
    task.task = (FP)mind_ui_task;
    task.itskpri = MIND_UI_TASK_PRIORITY;
    task.stksz = TRON_BUILD_MIND_UI_TASK_STACK_BYTES;
    task.bufptr = mind_ui_task_stack;
    id = tk_cre_tsk(&task);
    return id > 0 && tk_sta_tsk(id, 0) == E_OK;
}
#else
extern int mind_ui_host_display_start(uint32_t priority);
extern int mind_ui_host_task_start(uint32_t priority);

int mind_ui_start(mind_ui_t *ui)
{
    return ui != NULL && ui->initialized != 0u &&
        mind_ui_host_display_start(MIND_UI_TASK_PRIORITY) &&
        mind_ui_host_task_start(MIND_UI_TASK_PRIORITY);
}
#endif
