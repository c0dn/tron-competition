#include "mind_audio.h"

#include <string.h>

#if !defined(MIND_APPLICATION_HOST_TEST) || defined(MIND_AUDIO_HOST_REGISTERS)
#ifdef MIND_APPLICATION_HOST_TEST
typedef uint32_t UW;
extern void mind_audio_host_out_w(uint32_t address, uint32_t value);
#define out_w(address, value) mind_audio_host_out_w((address), (value))
#define MIND_AUDIO_POINTER_VALUE(pointer) ((uint32_t)(uintptr_t)(pointer))
#else
#include <tk/tkernel.h>
#define MIND_AUDIO_POINTER_VALUE(pointer) ((UW)(pointer))
#endif

#define MIND_AUDIO_PWM0_BASE 0x4001c000u
#define MIND_AUDIO_PWM_TASKS_STOP (MIND_AUDIO_PWM0_BASE + 0x004u)
#define MIND_AUDIO_PWM_TASKS_SEQSTART0 (MIND_AUDIO_PWM0_BASE + 0x008u)
#define MIND_AUDIO_PWM_ENABLE (MIND_AUDIO_PWM0_BASE + 0x500u)
#define MIND_AUDIO_PWM_MODE (MIND_AUDIO_PWM0_BASE + 0x504u)
#define MIND_AUDIO_PWM_COUNTERTOP (MIND_AUDIO_PWM0_BASE + 0x508u)
#define MIND_AUDIO_PWM_PRESCALER (MIND_AUDIO_PWM0_BASE + 0x50cu)
#define MIND_AUDIO_PWM_DECODER (MIND_AUDIO_PWM0_BASE + 0x510u)
#define MIND_AUDIO_PWM_SEQ0_PTR (MIND_AUDIO_PWM0_BASE + 0x520u)
#define MIND_AUDIO_PWM_SEQ0_CNT (MIND_AUDIO_PWM0_BASE + 0x524u)
#define MIND_AUDIO_GPIO_P0_PIN0_CNF 0x50000700u
#define MIND_AUDIO_PWM_PSEL_OUT0 (MIND_AUDIO_PWM0_BASE + 0x560u)

static uint16_t mind_audio_sample[1];

static void mind_audio_start_tone(uint16_t countertop)
{
    mind_audio_sample[0] = (uint16_t)(countertop / 2u);
    out_w(MIND_AUDIO_PWM_ENABLE, 1u);
    out_w(MIND_AUDIO_PWM_MODE, 0u);
    out_w(MIND_AUDIO_PWM_PRESCALER, 0u);
    out_w(MIND_AUDIO_PWM_COUNTERTOP, countertop);
    out_w(MIND_AUDIO_PWM_DECODER, 0u);
    out_w(MIND_AUDIO_PWM_SEQ0_PTR, MIND_AUDIO_POINTER_VALUE(mind_audio_sample));
    out_w(MIND_AUDIO_PWM_SEQ0_CNT, 1u);
    out_w(MIND_AUDIO_PWM_TASKS_SEQSTART0, 1u);
}
#endif

void mind_audio_init(mind_audio_t *audio)
{
    if (audio == NULL) {
        return;
    }
    memset(audio, 0, sizeof(*audio));
}

void mind_audio_hardware_init(void)
{
#if !defined(MIND_APPLICATION_HOST_TEST) || defined(MIND_AUDIO_HOST_REGISTERS)
    /* P0.00 is the micro:bit v2 built-in speaker.  DIR=output and INPUT=
     * disconnect; no scheduler or radio register is touched. */
    out_w(MIND_AUDIO_GPIO_P0_PIN0_CNF, 0x00000003u);
    out_w(MIND_AUDIO_PWM_PSEL_OUT0, 0u);
    out_w(MIND_AUDIO_PWM_TASKS_STOP, 1u);
#endif
}

void mind_audio_queue_root_cue(mind_audio_t *audio, uint32_t now_ms)
{
    if (audio == NULL) {
        return;
    }
    if (audio->phase != 0u && audio->queued_cues != 0xffu) {
        audio->queued_cues++;
    }
    audio->phase_started_ms = now_ms;
    audio->phase = 1u;
#if !defined(MIND_APPLICATION_HOST_TEST) || defined(MIND_AUDIO_HOST_REGISTERS)
    mind_audio_start_tone(16000u);
#endif
}

void mind_audio_service(mind_audio_t *audio, uint32_t now_ms)
{
    if (audio == NULL || audio->phase == 0u) {
        return;
    }
    if (audio->phase == 1u && (uint32_t)(now_ms - audio->phase_started_ms) >= 80u) {
        audio->phase = 2u;
#if !defined(MIND_APPLICATION_HOST_TEST) || defined(MIND_AUDIO_HOST_REGISTERS)
        mind_audio_start_tone(12800u);
#endif
    } else if (audio->phase == 2u &&
               (uint32_t)(now_ms - audio->phase_started_ms) >= 160u) {
        if (audio->queued_cues != 0u) {
            audio->queued_cues--;
            audio->phase_started_ms = now_ms;
            audio->phase = 1u;
#if !defined(MIND_APPLICATION_HOST_TEST) || defined(MIND_AUDIO_HOST_REGISTERS)
            mind_audio_start_tone(16000u);
#endif
        } else {
            audio->phase = 0u;
#if !defined(MIND_APPLICATION_HOST_TEST) || defined(MIND_AUDIO_HOST_REGISTERS)
            out_w(MIND_AUDIO_PWM_TASKS_STOP, 1u);
#endif
        }
    }
}
