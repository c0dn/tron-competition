#ifndef MIND_AUDIO_H
#define MIND_AUDIO_H

#include <stdint.h>

/* One nonblocking, application-owned P0.00/PWM0 cue. */
typedef struct mind_audio {
    uint32_t phase_started_ms;
    uint8_t queued_cues;
    uint8_t phase;
} mind_audio_t;

void mind_audio_init(mind_audio_t *audio);
/* UI-task-only hardware owner for P0.00/PWM0. */
void mind_audio_hardware_init(void);
void mind_audio_queue_root_cue(mind_audio_t *audio, uint32_t now_ms);
void mind_audio_service(mind_audio_t *audio, uint32_t now_ms);

#endif /* MIND_AUDIO_H */
