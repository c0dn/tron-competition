#ifndef MIND_UI_H
#define MIND_UI_H

#include <stdint.h>

#include "mind_audio.h"

#define MIND_UI_TASK_PRIORITY 12u

typedef struct mind_ui {
    mind_audio_t audio;
    uint8_t node_number;
    uint8_t initialized;
    /* Mesh producer brackets the copied desired state with an even sequence;
     * the priority-12 UI consumer retries until it observes one stable copy. */
    volatile uint32_t desired_guard;
    volatile uint32_t desired_transition_sequence;
    volatile uint8_t desired_local_root;
    volatile uint8_t desired_active_roots;
    uint32_t observed_transition_sequence;
    uint8_t producer_local_root;
    uint8_t local_root;
    uint8_t active_roots;
} mind_ui_t;

void mind_ui_init_state(mind_ui_t *ui, uint8_t node_number);
/* Mesh-only producer: copies desired state and monotonically publishes every
 * OFF->ON transition.  It never calls display/audio/PWM APIs. */
void mind_ui_publish(mind_ui_t *ui, uint8_t local_root, uint8_t active_roots);
/* UI-only consumer; caller provides real kernel time. */
void mind_ui_service(mind_ui_t *ui, uint32_t now_ms);
void mind_ui_render(const mind_ui_t *ui, uint32_t now_ms, uint8_t rows_out[5]);
int mind_ui_start(mind_ui_t *ui);

#endif /* MIND_UI_H */
