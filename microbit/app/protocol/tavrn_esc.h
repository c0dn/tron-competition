#ifndef TAVRN_ESC_H
#define TAVRN_ESC_H

#include <stdint.h>

#include "tavrn_gtt.h"

#define TAVRN_ESC_FIXED_K 1u

typedef char tavrn_esc_fixed_k_guard[(TAVRN_ESC_FIXED_K == 1u) ? 1 : -1];

typedef enum tavrn_esc_context_status {
    TAVRN_ESC_CONTEXT_UNIQUE = 0,
    TAVRN_ESC_CONTEXT_UNKNOWN,
    TAVRN_ESC_CONTEXT_RESERVED,
    TAVRN_ESC_CONTEXT_COLLIDING,
    TAVRN_ESC_CONTEXT_INVALID,
} tavrn_esc_context_status_t;

typedef struct tavrn_esc_context_match {
    tavrn_adva_t identity;
    uint8_t sid8;
} tavrn_esc_context_match_t;

/* Resolve only retained, non-departed identities.  Hard-expired liveness is
 * deliberately still context until maintenance purges it; departed tombstones
 * preserve membership history but never make a compressed identifier ambiguous. */
tavrn_esc_context_status_t tavrn_esc_resolve_sid8(
    const tavrn_gtt_t *gtt, uint8_t sid8, uint32_t now_ms,
    tavrn_esc_context_match_t *match_out);

#endif /* TAVRN_ESC_H */
